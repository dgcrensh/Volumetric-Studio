#pragma once

// Fields (spec, "Data types per edge"; plan, M10): signed distance or
// scalar functions over model space.
//
// A Field is an immutable tree of FieldExpr nodes (primitives at the
// leaves, booleans and maths above them), built by the functions below and
// evaluated lazily: nothing is computed until a point is asked for, so a
// graph can combine fields freely and pay only for the samples it takes.
//
// Conventions. Distances are in mm; signed distance fields are negative
// inside and positive outside. Booleans use min/max (exact union and
// intersection of the regions, a bound on the distance elsewhere); smooth
// variants blend within `radius` with the polynomial smooth minimum, which
// uses no libm calls, so samples are bit-identical across platforms.
// Evaluation is FP64; samples are stored as FP32 (the precision split).
//
// Each expression carries a digest of its whole tree (operation,
// parameters, children, and for mesh-distance leaves the mesh's digest), so
// two separately built but equal fields hash equal. The per-node cache and
// output hashes rely on that.
//
// Sampling (sample()) evaluates a field at the nodes of a regular grid
// covering a box, typically the boundary's bounding box: grid point
// (i, j, k) is origin + (i, j, k) * cell, and values run x fastest, then y,
// then z. Each sample is computed on its own, so splitting the grid across
// threads can't change any value.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "vs/core/box.hpp"
#include "vs/core/diagnostic.hpp"
#include "vs/core/hash.hpp"
#include "vs/core/vec.hpp"
#include "vs/eval/cancel.hpp"
#include "vs/eval/thread_pool.hpp"
#include "vs/proc/distance.hpp"
#include "vs/proc/types.hpp"

namespace vs::proc {

enum class FieldOp {
  Constant,            // value
  Sphere,              // centre, radius
  Box,                 // centre, half size (axis-aligned)
  Cylinder,            // centre, unit axis, radius, half height (capped)
  Plane,               // point, unit normal: unsigned distance to the plane
  HalfSpace,           // point, unit normal: signed, positive along the normal
  MeshSigned,          // signed distance to a closed mesh (a boundary)
  MeshUnsigned,        // distance to a mesh (a connection surface's patch)
  Union,               // min(a, b)
  Intersection,        // max(a, b)
  Difference,          // max(a, -b)
  SmoothUnion,         // radius
  SmoothIntersection,  // radius
  SmoothDifference,    // radius
  Offset,              // a - distance (grows the region by distance)
  Shell,               // max(a, -a - thickness): the inner skin of a region
  Negate,              // -a (the complement of a region)
  Abs,                 // |a| (unsigned distance to a's surface)
  Add,                 // a + b
  Multiply,            // a * b
  Scale,               // a * factor
};

class FieldExpr {
 public:
  [[nodiscard]] FieldOp op() const { return op_; }
  [[nodiscard]] const std::vector<double>& numbers() const { return numbers_; }
  [[nodiscard]] const std::vector<Field>& children() const { return children_; }
  [[nodiscard]] const Hash256& hash() const { return hash_; }

  // The field's value at `p`.
  [[nodiscard]] double eval(const dvec3& p) const;

  // Use the factory functions below.
  FieldExpr(FieldOp op, std::vector<double> numbers, std::vector<Field> children,
            std::shared_ptr<const TriangleDistance> mesh);

 private:
  FieldOp op_;
  std::vector<double> numbers_;
  std::vector<Field> children_;
  std::shared_ptr<const TriangleDistance> mesh_;
  Hash256 hash_{};
};

// --- factories (arguments already validated by the nodes) ---------------------------

Field constant_field(double value);
Field sphere_field(const dvec3& centre, double radius);
Field box_field(const dvec3& centre, const dvec3& size);
// `axis` need not be unit length (it is normalised; it must not be zero).
Field cylinder_field(const dvec3& centre, const dvec3& axis, double radius, double height);
Field plane_field(const dvec3& point, const dvec3& normal);
Field half_space_field(const dvec3& point, const dvec3& normal);
Field mesh_signed_field(std::shared_ptr<const TriangleDistance> mesh);
Field mesh_unsigned_field(std::shared_ptr<const TriangleDistance> mesh);
Field union_field(Field a, Field b);
Field intersection_field(Field a, Field b);
Field difference_field(Field a, Field b);
Field smooth_union_field(Field a, Field b, double radius);
Field smooth_intersection_field(Field a, Field b, double radius);
Field smooth_difference_field(Field a, Field b, double radius);
Field offset_field(Field a, double distance);
Field shell_field(Field a, double thickness);
Field negate_field(Field a);
Field abs_field(Field a);
Field add_field(Field a, Field b);
Field multiply_field(Field a, Field b);
Field scale_field(Field a, double factor);

// The polynomial smooth minimum and maximum the smooth booleans use.
double smooth_min(double a, double b, double radius);
double smooth_max(double a, double b, double radius);

// --- sampling ------------------------------------------------------------------------

struct GridSpec {
  dvec3 origin{};
  double cell = 1.0;  // mm between neighbouring grid points
  std::uint32_t nx = 0, ny = 0, nz = 0;

  [[nodiscard]] std::size_t size() const {
    return static_cast<std::size_t>(nx) * ny * nz;
  }
  [[nodiscard]] std::size_t index(std::uint32_t i, std::uint32_t j, std::uint32_t k) const {
    return (static_cast<std::size_t>(k) * ny + j) * nx + i;
  }
  [[nodiscard]] dvec3 point(std::uint32_t i, std::uint32_t j, std::uint32_t k) const;
  friend bool operator==(const GridSpec&, const GridSpec&) = default;
};
void hash_append(Hasher& h, const GridSpec& grid);

// The most samples a grid may have (2^24, 64 MB of FP32).
inline constexpr std::size_t kMaxGridSamples = std::size_t{1} << 24;

// The grid covering `box` with points `cell` apart, from box.min, enough
// of them that the last reaches box.max on each axis (it may pass it by
// less than a cell). `cell` 0 picks the cell giving 64 cells along the
// box's longest side. Fails with proc.sample.empty for an empty box or a
// cell that isn't positive and finite, and proc.sample.too_large past
// kMaxGridSamples.
core::Result<GridSpec> grid_covering(const dbox3& box, double cell);

struct SampledField {
  GridSpec grid;
  std::vector<float> values;  // grid.size() of them, x fastest

  [[nodiscard]] float at(std::uint32_t i, std::uint32_t j, std::uint32_t k) const {
    return values[grid.index(i, j, k)];
  }
  [[nodiscard]] Hash256 hash() const;
  friend bool operator==(const SampledField&, const SampledField&) = default;
};

// Samples `field` on `grid`, z slices in parallel on `pool` when given.
// A cancelled token stops early; the result is then incomplete and
// `cancelled` (when given) is set.
SampledField sample(const FieldExpr& field, const GridSpec& grid, eval::ThreadPool* pool = nullptr,
                    const eval::CancellationToken* cancel = nullptr, bool* cancelled = nullptr);

}  // namespace vs::proc

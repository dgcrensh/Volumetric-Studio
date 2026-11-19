#pragma once

// Measurements and topology queries on kernel shapes.

#include <memory>
#include <optional>
#include <vector>

#include "vs/core/box.hpp"
#include "vs/core/settings.hpp"
#include "vs/core/vec.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/shape.hpp"

namespace vs::kernel {

using core::dbox3;
using core::dvec3;
using core::ProductSettings;

struct MassProperties {
  double volume = 0.0;  // mm³
  dvec3 center_of_mass{};
};

// Volume and centre of gravity, assuming uniform density. Fails with
// "kernel.mass.empty" for a solid with no volume.
Result<MassProperties> mass_properties(const Solid& solid);

// Total area of the shape's faces, in mm².
double surface_area(const Shape& shape);

// Length of an edge, in mm.
double length(const Edge& edge);

// The tight axis-aligned box around the shape's exact geometry (not its
// control points or tolerances). Empty for an empty shape.
dbox3 bounding_box(const Shape& shape);

struct DistanceResult {
  double distance = 0.0;
  dvec3 point_on_a{};  // one closest pair; there may be others
  dvec3 point_on_b{};
};

// Minimum distance between two shapes; 0 when they touch or overlap (for
// solids, overlapping volumes count as distance 0).
Result<DistanceResult> min_distance(const Shape& a, const Shape& b);

enum class PointClass {
  Inside,
  Outside,
  OnBoundary,  // within the linear tolerance of the boundary
};

// Classifies a point against a solid, using the product's linear tolerance.
PointClass classify(const Solid& solid, const dvec3& point, const ProductSettings& settings);

// The same for many points against one solid: prepares the solid once, which
// is much faster than classify() per point. Copies share the prepared
// solid; one classifier must not be used from two threads at once.
class SolidClassifier {
 public:
  SolidClassifier(const Solid& solid, const ProductSettings& settings);
  [[nodiscard]] PointClass classify(const dvec3& point) const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

// Distance from a point to a shape: for a solid, 0 inside or on it and
// otherwise the distance to its surface; for a face or edge, to its nearest
// point.
Result<double> distance_to(const Shape& shape, const dvec3& point);

// Sub-shapes, each listed once, in the kernel's deterministic traversal order.
std::vector<Face> faces(const Shape& shape);

// The plane a planar face lies on, with the normal facing the way the face
// does (out of the material, for a face of a solid). nullopt for a face on
// any other kind of surface. `origin` is the face's centre of area.
std::optional<Plane> face_plane(const Face& face);

// A cylindrical face's surface: its axis (unit direction, origin on the
// axis), radius, and which way the face faces.
struct Cylinder {
  Axis axis;
  double radius = 0.0;
  // The face's normal points towards the axis (the wall of a hole) rather
  // than away from it (the outside of a boss).
  bool inward = false;
};
// nullopt for a face on any other kind of surface.
std::optional<Cylinder> face_cylinder(const Face& face);
std::vector<Edge> edges(const Shape& shape);
// True when `edge` is a seam of `face`: where a closed surface (a full
// cylinder, say) meets itself. Seams aren't real edges of the shape's
// outline and can't be filleted.
bool is_seam(const Edge& edge, const Face& face);
std::vector<Solid> solids(const Shape& shape);
std::size_t vertex_count(const Shape& shape);
// The positions of the shape's vertices, each once, in traversal order.
std::vector<dvec3> vertex_points(const Shape& shape);

}  // namespace vs::kernel

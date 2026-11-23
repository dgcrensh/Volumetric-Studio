#pragma once

// Measurements and topology queries on kernel shapes.

#include <memory>
#include <optional>
#include <span>
#include <utility>
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

// The least and greatest of dot(direction, p) over the shape's exact
// geometry, as bounding_box() measures it (L6: a drawing's dimension to
// a shape's extreme). `direction` needn't be unit length; along a
// principal axis this is exactly bounding_box()'s range. nullopt for an
// empty shape or a direction that is zero or not finite.
std::optional<std::pair<double, double>> extent(const Shape& shape, const dvec3& direction);

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
// is much faster than classify() per point. It isn't copyable (a copy would
// share the prepared solid's mutable state); one classifier must not be
// used from two threads at once, so give each thread its own, on its own
// deep_copy() of the solid.
class SolidClassifier {
 public:
  SolidClassifier(const Solid& solid, const ProductSettings& settings);
  ~SolidClassifier();
  SolidClassifier(const SolidClassifier&) = delete;
  SolidClassifier& operator=(const SolidClassifier&) = delete;
  SolidClassifier(SolidClassifier&&) noexcept;
  SolidClassifier& operator=(SolidClassifier&&) noexcept;
  [[nodiscard]] PointClass classify(const dvec3& point) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
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
// The same plane and normal, with `origin` the surface's own origin (some
// point of the infinite plane, not of the face) and no area integration:
// for comparing planes, where face_plane()'s centre costs most of the time
// (H5).
std::optional<Plane> face_surface_plane(const Face& face);

// The face's normals (facing the way the face does) at `points`, when every
// one of them lies within `tolerance` of the face's underlying surface (the
// whole surface, not only the trimmed face); nullopt otherwise, or for an
// empty list. Works on any kind of surface: feature face names use it for
// sides that are neither planes nor cylinders, such as an extruded Bézier's
// (decision 0040).
std::optional<std::vector<dvec3>> surface_normals_at(const Face& face,
                                                     std::span<const dvec3> points,
                                                     double tolerance);

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
// One edge of a face's outline, as the face's boundary runs along it.
// Lines and circular arcs (all a sketch makes) carry their exact geometry;
// any other curve is Other, with only its ends and length.
struct LoopEdge {
  enum class Kind { Line, Circle, Other };
  Kind kind = Kind::Other;
  dvec3 start{};  // where the loop enters the edge
  dvec3 end{};    // where it leaves; equal to start for a full circle
  double length = 0.0;  // mm
  // Circle only: the centre, the radius, and a unit axis about which the
  // edge turns counter-clockwise from start to end through `sweep`
  // radians, in (0, 2π] (2π for a full circle).
  dvec3 centre{};
  dvec3 axis{};
  double radius = 0.0;
  double sweep = 0.0;
};

// A closed loop of a face's boundary: its edges in order, each starting
// where the previous one ends. Seam edges of closed surfaces are left out.
struct FaceLoop {
  std::vector<LoopEdge> edges;
};

// The loops bounding a face, outer loop first, then its holes, in the
// kernel's deterministic order. Each loop's edges are chained (one starts
// where the previous one ends), but which way a loop runs is not
// specified: callers that care orient it themselves. Empty for an empty
// face.
std::vector<FaceLoop> face_loops(const Face& face);

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

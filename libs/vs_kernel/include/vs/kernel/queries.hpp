#pragma once

// Measurements and topology queries on kernel shapes.

#include <vector>

#include "vs/core/box.hpp"
#include "vs/core/settings.hpp"
#include "vs/core/vec.hpp"
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

// Sub-shapes, each listed once, in the kernel's deterministic traversal order.
std::vector<Face> faces(const Shape& shape);
std::vector<Edge> edges(const Shape& shape);
std::vector<Solid> solids(const Shape& shape);
std::size_t vertex_count(const Shape& shape);

}  // namespace vs::kernel

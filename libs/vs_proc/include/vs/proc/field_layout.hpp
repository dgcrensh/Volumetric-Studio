#pragma once

// Where each FieldOp keeps its numbers in FieldExpr::numbers() (H3).
//
// FieldExpr::eval() reads them here and vs_gpu's shader generator
// (glsl.cpp) reads the same positions from its storage buffer, so both
// take them from these constants rather than repeating literals; vs_gpu
// also static_asserts the LatticeCell and TpmsSurface ordinals its shaders
// compare against and sizes its BVH stack from distance.hpp's
// kMaxBvhDepth. Operations not listed keep a single number at 0 (smooth
// booleans' radius, offset, shell, scale), two at 0 and 1 (clamp) or a
// point at 0 and a vector at 3 (sphere: centre, radius at 3; box: centre,
// half size; plane, half space: point, unit normal).

#include <cstddef>

namespace vs::proc::field_layout {

// Cylinder: centre, unit axis, radius, half height.
inline constexpr std::size_t kCylinderCentre = 0;
inline constexpr std::size_t kCylinderAxis = 3;
inline constexpr std::size_t kCylinderRadius = 6;
inline constexpr std::size_t kCylinderHalfHeight = 7;
inline constexpr std::size_t kCylinderNumbers = 8;

// Lattice and Tpms: the cell kind or surface (as its enum's ordinal), the
// cell size, a cell corner, and the thickness (0 when a child grades it).
inline constexpr std::size_t kInfillKind = 0;
inline constexpr std::size_t kInfillCell = 1;
inline constexpr std::size_t kInfillOrigin = 2;
inline constexpr std::size_t kInfillThickness = 5;
inline constexpr std::size_t kInfillNumbers = 6;

// Instances: the template's radius, then per copy kInstanceStride numbers:
// the inverse frame's 3x3 (row major), the frame's origin and the copy's
// reach.
inline constexpr std::size_t kInstancesRadius = 0;
inline constexpr std::size_t kInstancesFirst = 1;
inline constexpr std::size_t kInstanceInverse = 0;
inline constexpr std::size_t kInstanceOrigin = 9;
inline constexpr std::size_t kInstanceReach = 12;
inline constexpr std::size_t kInstanceStride = 13;

// Voronoi (decision 0042): the wall thickness, the seed grid's corner,
// its cell size and its cells per axis (whole numbers), then for each of
// the nx * ny * nz cells (x fastest) the index of its first seed and, last,
// the seed count (so cell c's seeds are [starts[c], starts[c + 1])), then
// the seeds' x, y, z, sorted by cell and, within a cell, by input order.
inline constexpr std::size_t kVoronoiThickness = 0;
inline constexpr std::size_t kVoronoiOrigin = 1;
inline constexpr std::size_t kVoronoiCell = 4;
inline constexpr std::size_t kVoronoiDims = 5;
inline constexpr std::size_t kVoronoiStarts = 8;
inline constexpr std::size_t kVoronoiMaxDim = 128;

}  // namespace vs::proc::field_layout

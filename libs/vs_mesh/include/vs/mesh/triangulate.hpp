#pragma once

// Constrained Delaunay triangulation of a planar polygon with holes, in
// FP64. The seam library uses it for every connection surface's shared
// interior triangulation (seam.hpp); meshers may use it for their own
// planar faces (M8, M11).
//
// The domain is loops[0] (the outer boundary) minus loops[1..] (holes),
// each a closed polygon over indices into `points`, in either orientation.
// Every point not on a loop is a Steiner point: it must lie strictly
// inside the domain, and it becomes a vertex of the triangulation. Loops
// must be simple and must not touch each other or share points.
//
// The result covers the domain exactly: every loop segment is an edge of
// exactly one triangle, every point is used, and triangles wind
// counter-clockwise in the (x, y) plane. Away from loop segments it is
// Delaunay. The output is a pure function of the input (no randomness, no
// hashing of addresses), so two calls with the same points and loops give
// the same triangles in the same order.
//
// Codes:
//   mesh.triangulate.input            fewer than three points in a loop, a
//                                     bad index, a non-finite coordinate, or
//                                     a hole outside the outer loop or inside
//                                     another hole
//   mesh.triangulate.duplicate_point  two points coincide
//   mesh.triangulate.self_intersecting  loop segments cross or touch
//   mesh.triangulate.steiner_outside  a Steiner point isn't inside the domain
//                                     (on a loop segment: .failed)
//   mesh.triangulate.failed           numerical trouble (shouldn't happen
//                                     for valid input; please report it)

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/vec.hpp"
#include "vs/mesh/mesh.hpp"

namespace vs::mesh {

using Loop = std::vector<std::uint32_t>;
using Segment = std::array<std::uint32_t, 2>;

core::Result<std::vector<Triangle>> triangulate(std::span<const core::dvec2> points,
                                                const std::vector<Loop>& loops);

// The same with extra constrained segments inside the domain (M8: a seam's
// outline drawn across a larger face). Each becomes an edge of the
// triangulation without bounding the domain. A segment may end on loop
// points or other segments' ends but must not cross or touch anything
// else, nor repeat a loop segment; an end that isn't on a loop is a
// Steiner point.
core::Result<std::vector<Triangle>> triangulate(std::span<const core::dvec2> points,
                                                const std::vector<Loop>& loops,
                                                std::span<const Segment> segments);

// Twice the signed area of a closed polygon (positive counter-clockwise).
double signed_double_area(std::span<const core::dvec2> points, const Loop& loop);

}  // namespace vs::mesh

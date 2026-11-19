#pragma once

// Small meshers for the seam and weld tests. Each meshes one volume against
// the seams it touches, independently of the other volumes, the way M8's
// tessellator and M11's field mesher will: seam faces come from the seam
// (add_seam_vertices / add_seam_face), everything else is the mesher's
// own, at its own edge length.

#include <cstdint>
#include <random>
#include <vector>

#include "vs/core/id.hpp"
#include "vs/core/vec.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/mesh/seam.hpp"

namespace meshers {

using vs::core::dvec2;
using vs::core::dvec3;
using vs::core::Id;
using vs::mesh::Mesh;
using vs::mesh::Seam;
using vs::mesh::SeamOutline;

// A random right-handed frame: origin within ±`reach` mm, any orientation.
vs::sketch::SketchPlane random_frame(std::mt19937_64& rng, double reach);

// A random simple outline about the frame's origin: a star-shaped polygon
// with 6 to 40 corners, radii in [0.6, 1] x `size`, and up to `max_holes`
// holes (circles or small polygons) well inside it. Outer loop
// counter-clockwise, holes clockwise.
SeamOutline random_outline(std::mt19937_64& rng, const vs::sketch::SketchPlane& frame,
                           double size, int max_holes);

// A rectangle outline (counter-clockwise), centred at `centre`.
SeamOutline rectangle(const vs::sketch::SketchPlane& frame, dvec2 centre, double w, double h);

// One end of a prism: either a seam (its own vertices and triangulation)
// or the outline divided at `spacing` and triangulated by the mesher.
struct End {
  const Seam* seam = nullptr;
  const SeamOutline* outline = nullptr;  // when there is no seam
  double spacing = 0.0;
  dvec3 offset{};  // translation applied to the outline (no seam only)
};

// A prism between two ends with the same outline topology: `bottom` and
// `top` (top lies on the normal's side of bottom). Walls join each outline
// segment's division on one end to the other's.
Mesh prism(const End& bottom, const End& top, const Id& volume);

// A pyramid on a seam (no holes) with its apex `height` along `side`
// (+1: the frame normal's side, -1: the other).
Mesh pyramid(const Seam& seam, double height, int side, const Id& volume);

// A box below a seam: its top face is the seam's plane, `margin` wider than
// the seam's outline (no holes) all round, and `depth` deep. The top face
// is the seam plus the ring round it.
Mesh box_under(const Seam& seam, double margin, double depth, const Id& volume);

}  // namespace meshers

#pragma once

// Field to mesh (plan, M11): surface nets on the sampling grid, the
// boundary clip, and the seams. Decision 0011 has the reasoning.
//
// Surface nets. surface_nets() meshes the region where a sampled field is
// negative (exactly 0 counts as outside; outside the grid too, so a region
// reaching the grid's edge is closed there). It is surface nets on the
// grid's tetrahedra: each cube is split into six tetrahedra round its
// diagonal (the same way everywhere), each tetrahedron the surface crosses
// gets a vertex at the mean of where the surface crosses its edges, and
// each crossed edge gets a polygon joining the vertices of the tetrahedra
// round it, fanned into triangles. A tetrahedron's surface is always one
// disc, so the result is a closed 2-manifold for any samples: no edge has
// more than two triangles, no vertex is pinched. Then each cube's vertices
// are merged into one wherever that keeps it so (an edge collapse passing
// the link condition), which leaves about two triangles per crossed grid
// cell, as cube-based surface nets give, and extra vertices only in the
// tangled cubes where one vertex would pinch the surface. The output is
// FP64 (`FieldMesh`); to_mesh() rounds it to FP32. Everything runs in grid
// order (z slowest, x fastest), so the same samples always give the same
// mesh, bit for bit, on any number of threads.
//
// Volume contents. mesh_contents() turns a procedural volume's sampled
// field into the volume's mesh:
//
//   1. clip: the field is intersected (max) with the boundary's signed
//      distance, inset by kContentsInset, so the region stays inside;
//   2. surface nets on the clipped samples;
//   3. seams: for each seam joining the volume (seam_library.hpp), the
//      grid's triangles on the patch (facing out, within a cell of its
//      plane, inside the outline or within a cell of it; near the outline
//      also the rounding of the boundary's edge, two cells deep) are
//      replaced by the seam's own triangulation plus its hole fills (the
//      contents have no bolt holes), and the rim they leave is stitched to
//      the seam's outer loop by a strip that walks both loops together by
//      arc length;
//   4. containment: vertices (never seam vertices) move inward, against
//      the boundary's gradient, until every vertex and every triangle's
//      centroid is inside the boundary by the inset (on it, for triangles
//      touching a seam), judged from the FP32 positions as validate's
//      containment check judges them. Only vertices near concave edges of
//      the boundary ever move far: a triangle spanning a re-entrant edge
//      would otherwise cut the corner.
//
// The result is closed, conforms to every seam it stitched, and welds to
// whatever is on the other side (weld.hpp).
//
// A seam is stitched only when the contents cover its patch: the removed
// triangles must form one disc whose rim stays within three cells of the
// patch outline all the way round, both running the same way. Otherwise
// the volume keeps its grid surface there, closed but not conforming, and
// the warning proc.mesh.seam_uncovered says why; the weld then fails at
// that surface (mesh.weld.nonconforming).
//
// Codes (the volume last):
//   proc.mesh.seam_uncovered  (warning) the contents don't cover a seam's
//                             patch, so it isn't stitched (the surface first)
//   proc.mesh.empty           (warning) the contents are empty inside the
//                             boundary: no mesh
//   proc.mesh.invalid         the mesh isn't closed and manifold (a bug:
//                             the construction above should never allow it)

#include <cstddef>
#include <span>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/core/vec.hpp"
#include "vs/eval/thread_pool.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/mesh/seam.hpp"
#include "vs/proc/distance.hpp"
#include "vs/proc/field.hpp"
#include "vs/proc/inputs.hpp"

namespace vs::proc {

struct FieldMesh {
  std::vector<dvec3> positions;
  std::vector<mesh::Triangle> triangles;  // counter-clockwise seen from outside

  [[nodiscard]] bool empty() const { return triangles.empty(); }
};

// The surface of {samples < 0}.
FieldMesh surface_nets(const SampledField& samples);

// The mesh rounded to FP32, every triangle in `volume`.
mesh::Mesh to_mesh(const FieldMesh& field_mesh, const core::Id& volume);

// How far inside the boundary's tessellation contents are clipped: its
// deflection, so a facet of a concave curved face, which lies up to that
// far outside the exact surface, can't take the contents out with it.
inline constexpr double kContentsInset = kInputDeflection;

// max(samples, boundary's signed distance + inset) at every grid point, z
// slices on `pool` when given.
SampledField clip_to_boundary(const SampledField& samples, const TriangleDistance& boundary,
                              double inset = 0.0, eval::ThreadPool* pool = nullptr);

struct ContentsMesh {
  mesh::Mesh mesh;
  std::vector<core::Id> stitched;  // surfaces whose seams the mesh conforms to
  std::size_t moved_vertices = 0;  // moved inward by step 4
  core::Diagnostics diagnostics;

  [[nodiscard]] bool ok() const { return !diagnostics.has_errors(); }
};

// The mesh of a procedural volume's contents (steps 1 to 4 above).
// `samples` are the contents field on the grid covering the boundary;
// `seams` are the seams of the surfaces joining the volume; `volume` names
// it (its id goes on every triangle).
ContentsMesh mesh_contents(const SampledField& samples, const TriangleDistance& boundary,
                           std::span<const mesh::Seam> seams, const core::SubjectRef& volume,
                           eval::ThreadPool* pool = nullptr);

}  // namespace vs::proc

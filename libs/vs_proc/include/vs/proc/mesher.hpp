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
//      arc length. On a curved seam (L2) "on the patch" is along the
//      surface's normal at each point and inside its unrolled outline; a
//      band's removal leaves two rims, each stitched to the seam loop at
//      its height;
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/core/vec.hpp"
#include "vs/eval/cancel.hpp"
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

// Cancellation (H5): every function below that takes a CancellationToken
// checks it between its stages and its chunks (the merge once per wave)
// and, once cancelled, stops and returns an empty result. Callers check the
// token, never the result, to tell "cancelled" from "empty".

// The surface of {samples < 0}. With a pool, the passes run on it (L1b);
// the mesh is the same, bit for bit, on any number of threads.
FieldMesh surface_nets(const SampledField& samples, eval::ThreadPool* pool = nullptr,
                       const eval::CancellationToken* cancel = nullptr);

// --- surface nets in two steps (L1b; decision 0019) ------------------------------------
//
// The unmerged net is one vertex per crossed tetrahedron and the fanned
// polygon round each crossed grid edge, before any cube's vertices are
// merged. Every cube and every grid point is independent there, so it is
// made in z slabs on a pool, or by a MeshingBackend (the GPU), and laid
// out in grid order exactly as the serial loop lays it out.
//
// The merge is not independent: whether a collapse in one cube passes the
// link condition depends on collapses already made in the cubes round it.
// But only on those: a triangle's vertices lie in cubes sharing a grid
// edge, so cubes three apart touch nothing in common. merge_net() goes up
// the grid in slabs of whole cube layers, one after another, and inside a
// slab merges whole rows of cubes (along x, each in grid order) in waves,
// row (j, k) in wave j + 3k: every row that the serial order visits first
// and that touches it is in an earlier wave, and two rows of one wave are
// three rows or layers apart, so they run in parallel without locks. The
// result is the serial merge's, bit for bit. A slab only needs the fans
// (triangles round each vertex) of its own layers and one each side, and
// the vertices and triangles below it are compacted in place as it goes,
// so the merge needs little more memory than the net it is given
// (decision 0028).
struct UnmergedNet {
  std::vector<dvec3> sums;             // per vertex: the sum of its crossing points
  std::vector<std::uint8_t> counts;    // per vertex: how many (the vertex is sum / count)
  // Crossed cube c's vertices are [group_starts[c], group_starts[c + 1]);
  // one more entry than cubes.
  std::vector<std::uint32_t> group_starts;
  // Crossed cube c's index among the cubes of the grid padded by one
  // sample all round (x fastest), in increasing order.
  std::vector<std::uint64_t> cubes;
  std::vector<mesh::Triangle> triangles;  // counter-clockwise seen from outside

  friend bool operator==(const UnmergedNet&, const UnmergedNet&) = default;
};

// The unmerged net of {samples < 0}: the installed MeshingBackend's when
// it takes the grid, else unmerged_net_cpu().
UnmergedNet unmerged_net(const SampledField& samples, eval::ThreadPool* pool = nullptr,
                         const eval::CancellationToken* cancel = nullptr);
UnmergedNet unmerged_net_cpu(const SampledField& samples, eval::ThreadPool* pool = nullptr,
                             const eval::CancellationToken* cancel = nullptr);

// Merges each cube's vertices (see surface_nets() above) and compacts.
// `grid` is the grid the net was made on. `slab_layers` is how many cube
// layers a slab takes: the result is the same for any value (tests vary
// it); more costs more memory and gives the waves more rows.
inline constexpr std::size_t kMergeSlabLayers = 16;
FieldMesh merge_net(UnmergedNet net, const GridSpec& grid, eval::ThreadPool* pool = nullptr,
                    const eval::CancellationToken* cancel = nullptr,
                    std::size_t slab_layers = kMergeSlabLayers);

// M11's serial surface nets, unchanged: what surface_nets() must give.
// For tests and self-checks.
FieldMesh surface_nets_reference(const SampledField& samples);

// The mesh rounded to FP32, every triangle in `volume`.
mesh::Mesh to_mesh(const FieldMesh& field_mesh, const core::Id& volume);

// How far inside the boundary's tessellation contents are clipped: its
// deflection, so a facet of a concave curved face, which lies up to that
// far outside the exact surface, can't take the contents out with it.
inline constexpr double kContentsInset = kInputDeflection;

// max(samples, float(boundary's signed distance + inset)) at every grid
// point: the installed MeshingBackend's when it takes the grid, else
// clip_to_boundary_cpu() (z slices on `pool` when given).
SampledField clip_to_boundary(const SampledField& samples, const TriangleDistance& boundary,
                              double inset = 0.0, eval::ThreadPool* pool = nullptr,
                              const eval::CancellationToken* cancel = nullptr);
SampledField clip_to_boundary_cpu(const SampledField& samples, const TriangleDistance& boundary,
                                  double inset = 0.0, eval::ThreadPool* pool = nullptr,
                                  const eval::CancellationToken* cancel = nullptr);

// --- meshing backends (L1b) ------------------------------------------------------------
//
// Something that does the clip or the unmerged net faster than the CPU
// and gives exactly the CPU's result, bit for bit (vs_gpu installs one).
// Each is offered every grid first; what it declines, the CPU does, so
// results never depend on which one ran. Called from any thread.
class MeshingBackend {
 public:
  virtual ~MeshingBackend() = default;
  // Fills `values` (samples.grid.size() of them) and returns true, or
  // returns false to leave the clip to the CPU.
  virtual bool clip(const SampledField& samples, const TriangleDistance& boundary, double inset,
                    std::vector<float>& values) = 0;
  // Fills `net` and returns true, or returns false to leave it to the CPU.
  virtual bool unmerged_net(const SampledField& samples, UnmergedNet& net) = 0;
};
// For backends: how surface_nets() splits the grid. Cube corner c sits at
// (c & 1, (c >> 1) & 1, (c >> 2) & 1); tetrahedron t has corners
// tet_corners[t], lowest first; grid edges run from a point P to P +
// direction (corner bits) for the seven directions in order; round d lists
// the tetrahedra round such an edge in fan order, each by its cube's
// lowest corner relative to P and its index.
struct NetTables {
  struct RoundTet {
    std::array<std::int32_t, 3> offset;
    std::uint32_t tet;
  };
  std::array<std::array<std::uint32_t, 4>, 6> tet_corners;
  std::array<std::uint32_t, 7> directions;
  std::array<std::vector<RoundTet>, 7> rounds;
};
const NetTables& net_tables();

// Process-wide; null removes it. Not while meshes are being made.
void set_meshing_backend(std::shared_ptr<MeshingBackend> backend);
[[nodiscard]] std::shared_ptr<MeshingBackend> meshing_backend();

// True only if `mesh` is closed and manifold as mesh::analyse() judges it
// (no degenerate triangle; every half-edge once and its reverse once;
// every vertex's link one cycle), checked per vertex on `pool` (L1b:
// analyse() is serial). False means "look closer": analyse() says what's
// wrong, or that nothing is.
bool closed_manifold(const mesh::Mesh& mesh, eval::ThreadPool* pool = nullptr);

struct ContentsMesh {
  mesh::Mesh mesh;
  std::vector<core::Id> stitched;  // surfaces whose seams the mesh conforms to
  std::size_t moved_vertices = 0;  // moved inward by step 4
  core::Diagnostics diagnostics;
  // Stopped early by the token (H5): the mesh is empty and means nothing.
  bool cancelled = false;

  [[nodiscard]] bool ok() const { return !diagnostics.has_errors(); }
};

// The mesh of a procedural volume's contents (steps 1 to 4 above).
// `samples` are the contents field on the grid covering the boundary;
// `seams` are the seams of the surfaces joining the volume; `volume` names
// it (its id goes on every triangle).
ContentsMesh mesh_contents(const SampledField& samples, const TriangleDistance& boundary,
                           std::span<const mesh::Seam> seams, const core::SubjectRef& volume,
                           eval::ThreadPool* pool = nullptr,
                           const eval::CancellationToken* cancel = nullptr);

}  // namespace vs::proc

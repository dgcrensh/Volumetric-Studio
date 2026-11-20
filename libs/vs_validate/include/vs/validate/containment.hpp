#pragma once

// Containment (spec, design rule 5): contents may reach their volume's
// boundary but may not cross it by more than the tolerance. One function
// per kind of contents, for M9 to call after each generation; validate()
// (validate.hpp) calls the B-rep one for every volume model::build() filled.
//
// B-rep contents. The part of the contents outside the boundary is
// `contents − boundary` (kernel cut, fuzzy at the linear tolerance). It is
// reported when it reaches farther than the tolerance from the boundary;
// slivers along the boundary within the tolerance are touching, not
// crossing. The report gives the region's box, volume and how far it
// reaches (the largest distance of its vertices from the boundary).
//
// Mesh contents (FP32, M8 and M11). Every vertex, and every triangle's
// centroid, is classified against the boundary. A point counts as outside
// when it is farther from the boundary than the linear tolerance plus its
// FP32 rounding (2⁻²² of its largest coordinate, two units in the last
// place), since an FP32 mesh can't place a vertex any closer to the exact
// boundary. Sampling vertices and centroids misses a triangle that bulges
// out of a concave boundary between its samples; meshes from M11 are fine
// enough for that not to matter, and M8's conforming tessellation puts its
// vertices on the boundary.
//
// Large meshes (M12). Samples deep inside are certified in bulk: an octree
// over the samples classifies each node's centre and measures its distance
// to the boundary's faces, and a node whose samples all lie within that
// distance of an inside centre needs nothing more. The rest (near the
// boundary) are classified one by one, in fixed chunks that `parallel`
// may run on several threads (each chunk with its own classifier); the
// results are folded in chunk order, so they never depend on the threads.
//
// Codes (registry.hpp):
//   validate.containment   error  the volume (act on its contents)
//   validate.undecided     error  the kernel couldn't decide

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include "vs/core/box.hpp"
#include "vs/core/diagnostic.hpp"
#include "vs/core/settings.hpp"
#include "vs/core/vec.hpp"
#include "vs/kernel/shape.hpp"

namespace vs::validate {

// A triangle mesh to check, as M7's mesh types will hold one: positions and
// indexed triangles (three indices per triangle). vs_mesh sits above this
// module, so this is a view, not its type.
struct MeshView {
  std::span<const core::fvec3> positions;
  std::span<const std::uint32_t> indices;
  // Optional (L2): per triangle, how much farther its centroid may lie
  // outside than the tolerance allows. Near a curved seam a mesh's
  // triangles are the seam's chords of the shared surface, which stray
  // from it by up to the seam's deflection (vs_mesh's kSeamDeflection);
  // the engine allows that much there. Empty: no slack anywhere.
  std::span<const float> centroid_slack = {};
};

struct ContainmentResult {
  bool contained = true;
  core::dbox3 region;          // what lies outside, when not contained
  double outside_volume = 0;   // mm³ (B-rep only)
  double reach = 0;            // mm, how far outside the boundary it reaches
  std::size_t outside_points = 0;  // vertices and centroids outside (meshes only)
  core::Diagnostics diagnostics;   // empty when contained
};

// `volume` names the volume in the diagnostics (model::Document::volume_subject).
ContainmentResult check_containment(const kernel::Solid& boundary, const kernel::Solid& contents,
                                    const core::SubjectRef& volume,
                                    const core::ProductSettings& settings);
// Runs task(0) .. task(n - 1), in any order, on any threads (vs_eval's
// ThreadPool::run, say; vs_validate sits below it).
using ParallelFor = std::function<void(std::size_t n, const std::function<void(std::size_t)>& task)>;

ContainmentResult check_containment(const kernel::Solid& boundary, const MeshView& mesh,
                                    const core::SubjectRef& volume,
                                    const core::ProductSettings& settings,
                                    const ParallelFor& parallel = {});

}  // namespace vs::validate

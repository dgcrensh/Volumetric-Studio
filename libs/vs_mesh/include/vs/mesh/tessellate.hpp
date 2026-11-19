#pragma once

// Conforming B-rep tessellation (plan, M8): a volume's solid as a closed
// triangle mesh that uses its connection surfaces' seams exactly, so the
// weld (weld.hpp) can join it to its neighbours structurally.
//
// How (decision 0008). The kernel meshes the solid with OCCT's BRepMesh
// and says which point of each face's triangulation lies on which edge or
// vertex (kernel/tessellate.hpp). Then, for each seam joining the volume:
//
//   1. B-rep vertices on the seam's outline become seam vertices: exactly
//      when one is there, otherwise the nearest one along the outline
//      ("snapped": the mesh moves by at most half a seam piece there);
//   2. edges lying along the outline are divided at the seam's outline
//      vertices instead of BRepMesh's points (BRepMesh's are kept on parts
//      of the edge off the outline);
//   3. planar faces lying on the seam's plane and covering its patch are
//      re-triangulated in the seam's frame, constrained Delaunay
//      (triangulate.hpp) with the patch outline as extra segments: the
//      triangles inside the patch are replaced by the seam's own, and
//      each hole of the patch the face doesn't have is closed with the
//      seam's hole fill;
//   4. every other face with an edge whose division changed is
//      re-triangulated in its scaled (u, v) parameter plane: its new
//      boundary plus BRepMesh's interior points (those not too close to
//      the boundary), constrained Delaunay. Faces whose edges are as
//      BRepMesh divided them keep BRepMesh's triangles.
//
// Points are shared structurally: one mesh vertex per B-rep vertex, per
// point of an edge's division and per seam vertex, so the mesh is closed
// by construction; analyse() confirms it. Seam vertices carry their ids
// and the seam's FP32 positions.
//
// Codes (the volume first unless said otherwise):
//   kernel.tessellate.*              passed through
//   mesh.tessellate.seam_uncovered   (surface, volume) no face of the solid
//                                    covers the seam's whole patch, so the
//                                    volume can't conform to it
//   mesh.tessellate.seam_conflict    (warning) a point lies on two seams;
//                                    the first one keeps it
//   mesh.tessellate.snapped          (info) how many vertices step 1 moved
//   mesh.tessellate.face_failed      a face couldn't be re-triangulated
//                                    (mesh.triangulate.* in the message)
//   mesh.tessellate.open             the volume's mesh isn't closed and
//                                    manifold (shouldn't happen; please report it)

#include <span>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/settings.hpp"
#include "vs/kernel/shape.hpp"
#include "vs/kernel/tessellate.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/mesh/seam.hpp"

namespace vs::mesh {

struct TessellationOptions {
  kernel::TessellationParams params;  // deflection and angle, as BRepMesh takes them
};

struct VolumeMesh {
  Mesh mesh;
  // The surfaces whose seams the mesh uses (in the order given).
  std::vector<core::Id> seams;
  core::Diagnostics diagnostics;

  [[nodiscard]] bool ok() const { return !diagnostics.has_errors(); }
};

// Tessellates `solid`, the geometry of `volume` (a subject naming it; its
// id goes on every triangle), conforming to `seams`: those of the
// connection surfaces that join it.
VolumeMesh tessellate(const kernel::Solid& solid, const core::SubjectRef& volume,
                      std::span<const Seam* const> seams, const core::ProductSettings& settings,
                      const TessellationOptions& options = {});

}  // namespace vs::mesh

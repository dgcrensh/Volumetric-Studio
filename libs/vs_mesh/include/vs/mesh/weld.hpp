#pragma once

// Welding (spec, "Combined mesh"): concatenate per-volume meshes and join
// them at their seams, structurally, with no booleans and no distance
// tests.
//
// weld() takes meshes whose vertices on seams carry their seam vertex ids
// (seam.hpp: add_seam_vertices()), and
//
//   1. checks every mesh (Mesh::check) and every seam vertex: its id must
//      be in the library and its position must be the seam's position32,
//      exactly;
//   2. checks conformity: for each seam and each volume in the input that
//      the seam joins, the volume must have every triangle of the seam's
//      triangulation (by seam vertex ids), once, all facing one way. A
//      volume the seam joins that doesn't mesh it at all fails too. Other
//      triangles joining three seam vertices (a ring round a concave
//      outline, outside the patch) are the mesher's own business;
//   3. concatenates the meshes, merging vertices that carry the same seam
//      vertex id (and only those);
//   4. removes internal faces: a seam triangle that two different volumes
//      both have, facing opposite ways, is where they are bonded, so both
//      copies go. A one-sided surface (a volume and the outside) keeps its
//      triangles;
//   5. drops vertices no triangle uses (the patch's interior) and checks
//      the result is watertight and manifold (topology.hpp).
//
// The result keeps each triangle's volume id. It is reported even when
// there are errors, for inspection; ok() says whether it can be used.
//
// Codes. A problem on a seam names the connection surface first, then the
// volume; elsewhere the volume comes first.
//   mesh.invalid                 a mesh breaks its invariants (mesh.hpp)
//   mesh.weld.unknown_vertex     a vertex carries an id the library doesn't know
//   mesh.weld.moved              a seam vertex isn't at the seam's position
//   mesh.weld.nonconforming      a volume's mesh of a seam isn't the seam's
//                                triangulation (or is missing)
//   mesh.weld.not_joined         a volume meshes a seam of a surface that
//                                doesn't join it
//   mesh.weld.open               the welded mesh has boundary edges
//   mesh.weld.non_manifold       non-manifold edges or vertices, or
//                                degenerate triangles
//   mesh.weld.orientation        neighbouring triangles disagree on orientation

#include <cstddef>
#include <span>

#include "vs/core/diagnostic.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/mesh/seam_library.hpp"
#include "vs/mesh/topology.hpp"

namespace vs::mesh {

struct WeldResult {
  Mesh mesh;
  TopologyReport topology;
  std::size_t merged_vertices = 0;     // input vertices folded into another one
  std::size_t internal_triangles = 0;  // seam triangles removed in pairs
  core::Diagnostics diagnostics;

  [[nodiscard]] bool ok() const { return !diagnostics.has_errors(); }
};

WeldResult weld(std::span<const Mesh> meshes, const SeamLibrary& seams);

}  // namespace vs::mesh

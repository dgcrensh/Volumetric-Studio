#pragma once

// Baking (spec, "Baking": "a procedural volume can be baked into a static
// volume holding its current output. It can also be unbaked if the graph
// is kept"; plan, M12).
//
// bake() turns a procedural volume into a static one holding `mesh` (its
// generated contents: proc::contents_mesh() of the engine's result) as a
// 3MF blob of the project, and keeps the graph's JSON on the static
// contents (model::StaticContents::baked_graph). Nothing else about the
// volume changes: boundary, parameters, seed, outputs, connections. The
// mesh was made against the volume's seams, and a 3MF file keeps its FP32
// positions exactly, so combine() finds its seam vertices again by
// position (mesh::attach_seam_vertices()) and the baked volume welds as
// the procedural one did. Static contents never regenerate, so a baked
// volume costs nothing to rebuild; its Published outputs lose their value
// (only a graph publishes numbers).
//
// unbake() restores the procedural contents from the kept graph and
// removes the blob when no other volume uses it, so baking then unbaking
// gives the project back exactly: the same document, blobs and
// model::content_hash().
//
// Both go through model::edit(), so a change the document's checks refuse
// isn't made.
//
// Codes (the volume first):
//   proc.bake.kind        bake: not a procedural volume (or keep-out);
//                         unbake: not a baked volume (static without a graph)
//   proc.bake.empty       bake: the mesh is empty
//   proc.bake.write       bake: the 3MF couldn't be written (mesh.io.* in the message)
//   model.reference.*     the volume isn't in the document

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/model/project.hpp"

namespace vs::proc {

core::Diagnostics bake(model::Project& project, const core::Id& volume, const mesh::Mesh& mesh);

core::Diagnostics unbake(model::Project& project, const core::Id& volume);

// True for static contents that were baked (and so can be unbaked).
bool is_baked(const model::Volume& volume);

}  // namespace vs::proc

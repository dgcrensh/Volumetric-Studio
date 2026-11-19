#pragma once

// The combined mesh of a whole design (spec, "Combined mesh"; plan, M8):
// tessellate each volume, concatenate, weld.
//
// combine() takes a built document (model::build()) and, for every volume
// that isn't a keep-out:
//
//   - B-rep contents (a parametric tree, an imported STEP or IGES file)
//     are tessellated against the seams of the surfaces joining the volume
//     (tessellate.hpp), so they weld to their neighbours;
//   - static mesh contents (an imported STL, 3MF or OBJ blob) are read as
//     they are. They know nothing of seams, so a seam joining one fails the
//     weld as non-conforming, naming the surface;
//   - a placeholder is left out, or with `placeholders` set its boundary is
//     tessellated instead (a blocked-out layout, for a quick print);
//   - generated meshes (procedural contents, M11) come from the caller,
//     which made them (vs_eval's engine, from vs_proc's generator) already
//     conforming to the seams: `generated` maps a volume to its mesh;
//   - anything else (procedural contents with no mesh, or a volume whose
//     build failed) is left out with an info.
//
// The seams come from build_seams(); the weld (weld.hpp) joins everything
// and checks it is watertight and manifold. Each volume's own mesh is
// kept too: closed on its own, it is what a per-volume 3MF writes.
//
// Codes (the volume first):
//   mesh.combine.skipped      (info) a volume isn't in the combined mesh, and why
//   mesh.combine.blob         a static mesh's blob is missing (no blob store
//                             was given, or it isn't in it)
//   mesh.combine.empty        no volume has geometry to mesh
//   mesh.io.*, mesh.tessellate.*, mesh.seam.*, mesh.weld.*   passed through

#include <map>
#include <string>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/settings.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/mesh/seam_library.hpp"
#include "vs/mesh/tessellate.hpp"
#include "vs/mesh/weld.hpp"
#include "vs/model/build.hpp"
#include "vs/model/document.hpp"
#include "vs/model/project.hpp"

namespace vs::mesh {

struct CombineOptions {
  TessellationOptions tessellation;
  // Mesh placeholder volumes' boundaries instead of leaving them out.
  bool placeholders = false;
};

struct VolumeTessellation {
  core::Id volume;
  std::string name;
  Mesh mesh;                     // closed on its own
  std::vector<core::Id> seams;   // surfaces whose seams it uses
};

struct CombinedMesh {
  SeamLibrary seams;
  std::vector<VolumeTessellation> volumes;  // in document order, those meshed
  WeldResult weld;                          // weld.mesh is the combined mesh
  // The seams', each volume's, then the weld's diagnostics.
  core::Diagnostics diagnostics;

  [[nodiscard]] bool ok() const { return !diagnostics.has_errors(); }
};

// Meshes made outside vs_mesh, by volume (see above).
using GeneratedMeshes = std::map<core::Id, const Mesh*>;

CombinedMesh combine(const model::Document& document, const model::DocumentBuild& build,
                     const model::BlobStore* blobs, const core::ProductSettings& settings,
                     const CombineOptions& options = {}, const GeneratedMeshes& generated = {});

}  // namespace vs::mesh

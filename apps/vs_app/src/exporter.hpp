#pragma once

// File > Export: the design as one welded mesh (STL) or one object per
// volume (3MF), from an evaluation the app already shows. No Qt.
//
// The weld is vs_mesh's combine() over the evaluation's build, with the
// meshes procedural volumes generated passed in as they are, exactly as
// the engine's own weld (vs_eval engine.cpp) does. Export refuses a design
// whose weld has errors (not watertight, not manifold, a seam that doesn't
// conform...): those are returned so the diagnostics list can show them.

#include <filesystem>
#include <string_view>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/settings.hpp"
#include "vs/eval/engine.hpp"
#include "vs/mesh/combine.hpp"
#include "vs/mesh/io.hpp"
#include "vs/model/project.hpp"

namespace vs::app {

enum class ExportFormat { Stl, ThreeMf };
std::string_view extension(ExportFormat format);

struct ExportResult {
  mesh::Bytes bytes;            // empty when export was refused
  core::Diagnostics diagnostics;  // the weld's, then the writer's
  std::size_t triangles = 0;
  std::size_t objects = 0;      // 3MF objects (one per meshed volume)

  [[nodiscard]] bool ok() const { return !bytes.empty() && !diagnostics.has_errors(); }
};

ExportResult export_mesh(const eval::Evaluation& evaluation, const model::BlobStore& blobs,
                         const core::ProductSettings& settings, ExportFormat format);

// The two halves of export_mesh() (H5), so a weld made once (by
// check_for_export(), ship.hpp) serves the export too: the combined mesh
// of `evaluation` (app.export.unchecked when it wasn't checked), and the
// file of a combined mesh made from that same evaluation.
mesh::CombinedMesh combine_for_export(const eval::Evaluation& evaluation, const model::BlobStore& blobs,
                                      const core::ProductSettings& settings);
ExportResult export_mesh(const eval::Evaluation& evaluation, const mesh::CombinedMesh& combined,
                         ExportFormat format);

// Writes `bytes` to `path` (replacing it). Diagnoses app.export.io.
core::Diagnostics write_file(const std::filesystem::path& path, const mesh::Bytes& bytes);

}  // namespace vs::app

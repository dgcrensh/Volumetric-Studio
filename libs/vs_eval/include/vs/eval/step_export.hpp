#pragma once

// STEP export of an evaluated design (M17; spec, "Output and interop").
//
// The file is one AP242 assembly named after the document, with one part
// per volume (keep-outs excluded), in document order, named after the
// volume:
//
//   parametric or static B-rep contents   the contents' solid (B-rep)
//   static mesh contents (STL, 3MF, OBJ)  a mesh body (kernel::mesh_shape)
//   procedural contents                   a mesh body, or with
//                                         ProceduralAs::Boundary the
//                                         volume's boundary (B-rep)
//   no contents (a placeholder)           the boundary (B-rep), with
//                                         eval.export.placeholder (info)
//
// Mesh bodies are exactly the triangles the engine generated (or the file
// held), in FP32 widened to FP64. Optionally the layout's own authored
// geometry follows: every volume's boundary ("<volume> boundary") and every
// connection surface's patch (a face, named after the surface).
//
// Only current results ship: a volume that is Failed, Stale or still
// regenerating (or shows stale contents) stops the export with
// eval.export.not_current, naming it. Writing is kernel::write_step()
// (kernel.step.* passed through).

#include <cstddef>
#include <filesystem>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/eval/engine.hpp"
#include "vs/kernel/exchange.hpp"

namespace vs::eval {

enum class ProceduralAs { Mesh, Boundary };

struct StepExportOptions {
  ProceduralAs procedural = ProceduralAs::Mesh;
  bool boundaries = false;           // also each volume's boundary
  bool connection_surfaces = false;  // also each connection surface's patch
};

struct StepExport {
  std::vector<kernel::NamedShape> parts;  // empty when refused
  Diagnostics diagnostics;
  std::size_t breps = 0;       // volume parts written as B-rep contents
  std::size_t meshes = 0;      // ... as mesh bodies
  std::size_t boundaries = 0;  // ... as their boundary (placeholders, procedural by choice)

  [[nodiscard]] bool ok() const { return !parts.empty() && !diagnostics.has_errors(); }
};

// The parts, without writing anything.
StepExport step_parts(const Evaluation& evaluation, const StepExportOptions& options = {});

// step_parts(), then the file. Returns the parts' diagnostics, then the
// writer's.
StepExport export_step(const Evaluation& evaluation, const std::filesystem::path& path,
                       const StepExportOptions& options = {});

}  // namespace vs::eval

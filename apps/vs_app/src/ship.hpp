#pragma once

// Model > Validate and the check before every export (M17; spec, "Ship:
// validate, then export"). No Qt.
//
// check_for_export() runs every check the studio has against what the app
// shows, and sums it up:
//
//   1. validate::validate(): the authoring rules, the layout build, overlap,
//      undeclared contact, connection type rules and B-rep containment;
//   2. the shown evaluation's per-volume diagnostics: generation, node
//      failures, containment of generated and imported meshes (eval.*,
//      proc.*, validate.containment);
//   3. any volume that isn't current (app.validate.stale: still
//      regenerating, cancelled or blocked; Failed volumes already carry
//      their errors);
//   4. the weld of the combined mesh (mesh.*), as STL and 3MF export would
//      make it. A design with nothing to mesh (only placeholders) isn't an
//      error here: STEP export can still ship its boundaries.
//
// The same diagnostic reported by two of these (validate() and the engine
// both see the layout) is listed once. Errors stop an export; warnings are
// shown and the export may go ahead.

#include <cstddef>
#include <string>

#include "vs/core/diagnostic.hpp"
#include "vs/core/settings.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/project.hpp"

namespace vs::app {

struct ShipCheck {
  core::Diagnostics diagnostics;  // in the order above
  std::size_t errors = 0;
  std::size_t warnings = 0;
  std::size_t volumes = 0;  // volumes checked (keep-outs included)
  std::size_t current = 0;  // ... of which current

  [[nodiscard]] bool ok() const { return errors == 0; }
};

ShipCheck check_for_export(const model::Project& project, const eval::Evaluation& evaluation,
                           const core::ProductSettings& settings);

// One short paragraph for the summary dialog and the status bar: whether
// the design is ready to export, the counts, and the first error.
std::string summary(const ShipCheck& check);

}  // namespace vs::app

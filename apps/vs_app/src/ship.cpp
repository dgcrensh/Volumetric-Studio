#include "ship.hpp"

#include <set>
#include <tuple>

#include <fmt/format.h>

#include "exporter.hpp"
#include "vs/validate/validate.hpp"

namespace vs::app {

ShipCheck check_for_export(const model::Project& project, const eval::Evaluation& evaluation,
                           const core::ProductSettings& settings) {
  ShipCheck out;
  std::set<std::tuple<int, std::string, std::string>> seen;
  const auto add = [&](const core::Diagnostic& d) {
    if (!seen.emplace(static_cast<int>(d.severity), d.code, d.message).second) return false;
    out.diagnostics.add(d);
    return true;
  };

  const validate::ValidationReport report = validate::validate(project.document, settings, &project.blobs);
  for (const auto& d : report.diagnostics) add(d);

  const model::Document& doc = evaluation.document;
  for (std::size_t i = 0; i < evaluation.volumes.size(); ++i) {
    const eval::VolumeResult& r = evaluation.volumes[i];
    ++out.volumes;
    for (const auto& d : r.diagnostics) add(d);
    if (r.status == model::VolumeStatus::Current && !r.stale_contents) {
      ++out.current;
    } else if (r.status != model::VolumeStatus::Failed) {
      const std::string name = i < doc.volumes.size() ? doc.volumes[i].name : r.volume.to_string();
      add(core::Diagnostic::warning(
          "app.validate.stale",
          fmt::format("Volume '{}' is {}: regenerate it (F5) before exporting", name,
                      model::to_string(r.status)),
          {doc.volume_subject(r.volume)}));
    }
  }

  if (evaluation.checked) {
    // The weld STL and 3MF export would make (its errors only matter
    // there), kept for the export to reuse (H5: it used to write the STL
    // here too, and weld again to export).
    auto combined = std::make_shared<mesh::CombinedMesh>(combine_for_export(evaluation, project.blobs, settings));
    for (const auto& d : combined->diagnostics) {
      if (d.code == "mesh.combine.empty") continue;
      if (add(d) && d.is_error()) ++out.weld_errors;
    }
    out.combined = std::move(combined);
  }

  out.errors = out.diagnostics.count(core::Severity::Error);
  out.warnings = out.diagnostics.count(core::Severity::Warning);
  return out;
}

std::string summary(const ShipCheck& check, ShipTarget target) {
  if (check.ok()) {
    return check.warnings == 0
               ? fmt::format("Ready to export: every check passed for {} volume(s).", check.volumes)
               : fmt::format("Ready to export, with {} warning(s) to review.", check.warnings);
  }
  if (check.ok_for(target)) {
    return fmt::format("Ready to export STEP. The mesh doesn't weld ({} error(s)), so STL and 3MF "
                       "export would stop{}.",
                       check.weld_errors,
                       check.warnings > 0 ? fmt::format("; {} warning(s) to review", check.warnings) : "");
  }
  // The first error that stops this export.
  std::string first;
  const bool weld_only = target == ShipTarget::Step;
  for (const auto& d : check.diagnostics) {
    if (!d.is_error()) continue;
    if (weld_only && d.code.starts_with("mesh.")) continue;
    first = d.message;
    break;
  }
  return fmt::format("Not ready to export: {} error(s){}. First: {}", check.errors,
                     check.warnings > 0 ? fmt::format(", {} warning(s)", check.warnings) : "", first);
}

}  // namespace vs::app

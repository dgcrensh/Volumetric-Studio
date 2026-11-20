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
    if (!seen.emplace(static_cast<int>(d.severity), d.code, d.message).second) return;
    out.diagnostics.add(d);
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
    // The weld STL and 3MF export would make (its errors only matter there).
    const ExportResult weld = export_mesh(evaluation, project.blobs, settings, ExportFormat::Stl);
    for (const auto& d : weld.diagnostics) {
      if (d.code != "mesh.combine.empty") add(d);
    }
  }

  out.errors = out.diagnostics.count(core::Severity::Error);
  out.warnings = out.diagnostics.count(core::Severity::Warning);
  return out;
}

std::string summary(const ShipCheck& check) {
  if (check.ok()) {
    return check.warnings == 0
               ? fmt::format("Ready to export: every check passed for {} volume(s).", check.volumes)
               : fmt::format("Ready to export, with {} warning(s) to review.", check.warnings);
  }
  std::string first;
  for (const auto& d : check.diagnostics) {
    if (d.is_error()) {
      first = d.message;
      break;
    }
  }
  return fmt::format("Not ready to export: {} error(s){}. First: {}", check.errors,
                     check.warnings > 0 ? fmt::format(", {} warning(s)", check.warnings) : "", first);
}

}  // namespace vs::app

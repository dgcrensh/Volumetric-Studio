#include "validate_commands.hpp"
#include "files.hpp"
#include "config_commands.hpp"

#include <chrono>
#include <filesystem>
#include <string>

#include <fmt/format.h>

#include "vs/core/settings.hpp"
#include "vs/model/project.hpp"
#include "vs/validate/validate.hpp"

namespace vs::cli {

int run_validate(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  const bool with_config = args.size() == 3 && args[1] == "--config";
  if (args.size() != 1 && !with_config) {
    err << "usage: vstudio-cli validate <project.vsproj> [--config <name>]\n";
    return 2;
  }
  const std::filesystem::path path = to_path(args[0]);
  auto loaded = model::load_project(path, /*check_document=*/false);
  if (!loaded) {
    err << loaded.error().to_string() << "\n";
    return 1;
  }
  if (with_config && !use_configuration(*loaded, args[2], err)) return 1;
  const auto settings = core::ProductSettings::defaults();
  const auto t0 = std::chrono::steady_clock::now();
  const auto report = validate::validate(loaded->document, settings, &loaded->blobs);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  const auto& d = report.diagnostics;
  out << fmt::format("project     {}\n", loaded->document.name);
  if (const auto* c = loaded->document.find_configuration(loaded->document.active_configuration)) {
    out << fmt::format("configuration {}\n", c->name);
  }
  out << fmt::format("volumes     {}, connection surfaces {}\n", loaded->document.volumes.size(),
                     loaded->document.connections.size());
  out << fmt::format("checked     in {:.0f} ms\n", ms);
  out << fmt::format("diagnostics {} errors, {} warnings, {} notes\n",
                     d.count(core::Severity::Error), d.count(core::Severity::Warning),
                     d.count(core::Severity::Info));
  for (const auto& r : report.routed) {
    out << r.diagnostic.to_string() << "\n";
    std::string where = fmt::format("  act on {}", validate::to_string(r.target.on));
    if (!r.target.subject.label.empty() && r.target.on != validate::ActOn::Layout) {
      where += fmt::format(": {}", r.target.subject.label);
    } else if (r.target.subject.kind != core::SubjectKind::Layout) {
      where += fmt::format(" ({})", r.target.subject.label);
    }
    if (!r.target.volume.is_null()) {
      where += fmt::format(", in '{}'", loaded->document.volume_subject(r.target.volume).label);
    }
    out << where << "\n";
  }
  out << (report.ok() ? (d.empty() ? "result      valid\n" : "result      valid, with warnings\n")
                      : "result      invalid\n");
  return report.ok() ? 0 : 1;
}

}  // namespace vs::cli

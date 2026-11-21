#include "sketch_commands.hpp"
#include "files.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <fmt/format.h>

#include "vs/core/settings.hpp"
#include "vs/sketch/profile.hpp"
#include "vs/sketch/serialize.hpp"
#include "vs/sketch/solver.hpp"

namespace vs::cli {

int run_sketch_solve(std::span<const std::string_view> args, std::ostream& out,
                     std::ostream& err) {
  if (args.empty() || args.size() > 2) {
    err << "usage: vstudio-cli sketch-solve <in.json> [<out.json>]\n";
    return 2;
  }
  std::ifstream in(to_path(args[0]), std::ios::binary);
  if (!in) {
    err << "vstudio-cli: can't read '" << args[0] << "'\n";
    return 1;
  }
  std::ostringstream text;
  text << in.rdbuf();
  auto sketch = sketch::sketch_from_json(text.str());
  if (!sketch) {
    err << sketch.error().to_string() << "\n";
    return 1;
  }

  const auto settings = core::ProductSettings::defaults();
  const auto report = sketch::solve(*sketch, settings);
  out << fmt::format("sketch '{}': {} entities, {} constraints\n", sketch->label(sketch->id()),
                     sketch->entities().size(), sketch->constraints().size());
  out << fmt::format("  status          {}\n", sketch::to_string(report.status));
  out << fmt::format("  dof             {}\n", report.dof);
  for (const auto& d : report.diagnostics) out << "  " << d.to_string() << "\n";
  if (!report.ok()) return 1;

  const auto profiles = sketch::extract_profiles(*sketch, settings);
  if (!profiles) {
    err << profiles.error().to_string() << "\n";
    return 1;
  }
  out << fmt::format("  profiles        {}\n", profiles->size());
  for (std::size_t i = 0; i < profiles->size(); ++i) {
    const auto& p = (*profiles)[i];
    auto face = sketch::make_face(*sketch, p, settings);
    out << fmt::format("    {}: area {:.6f} mm², {} hole{}, face {}\n", i + 1, p.area(),
                       p.holes.size(), p.holes.size() == 1 ? "" : "s",
                       face ? "valid" : face.error().code);
    if (!face) return 1;
  }

  if (args.size() == 2) {
    std::ofstream file(to_path(args[1]), std::ios::binary);
    file << sketch::to_json(*sketch);
    if (!file) {
      err << "vstudio-cli: can't write '" << args[1] << "'\n";
      return 1;
    }
    out << "wrote " << args[1] << "\n";
  }
  return 0;
}

}  // namespace vs::cli

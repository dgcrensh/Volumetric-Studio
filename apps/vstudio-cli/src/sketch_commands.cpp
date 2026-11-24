#include "sketch_commands.hpp"
#include "files.hpp"

#include <charconv>
#include <filesystem>
#include <optional>
#include <system_error>
#include <fstream>
#include <sstream>
#include <string>

#include <fmt/format.h>

#include "vs/core/settings.hpp"
#include "vs/sketch/profile.hpp"
#include "vs/sketch/serialize.hpp"
#include "vs/sketch/solver.hpp"
#include "vs/sketch/svg.hpp"

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

namespace {

// Prints a sketch's profiles and whether each makes a valid face. False when
// they don't extract or a face fails.
bool print_profiles(const sketch::Sketch& sk, const core::ProductSettings& settings,
                    std::ostream& out, std::ostream& err) {
  const auto profiles = sketch::extract_profiles(sk, settings);
  if (!profiles) {
    err << profiles.error().to_string() << "\n";
    return false;
  }
  out << fmt::format("  profiles        {}\n", profiles->size());
  for (std::size_t i = 0; i < profiles->size(); ++i) {
    const auto& p = (*profiles)[i];
    auto face = sketch::make_face(sk, p, settings);
    out << fmt::format("    {}: area {:.6f} mm², {} hole{}, face {}\n", i + 1, p.area(),
                       p.holes.size(), p.holes.size() == 1 ? "" : "s",
                       face ? "valid" : face.error().code);
    if (!face) return false;
  }
  return true;
}

std::optional<double> number_arg(std::string_view text) {
  double v = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc() || end != text.data() + text.size()) return std::nullopt;
  return v;
}

}  // namespace

int run_sketch_import_svg(std::span<const std::string_view> args, std::ostream& out,
                          std::ostream& err) {
  constexpr std::string_view kUsage =
      "usage: vstudio-cli sketch-import-svg <file.svg> [--out <sketch.json>] [--scale k] "
      "[--dpi n] [--gap mm] [--fix] [--no-smooth]\n";
  std::optional<std::string_view> input, output;
  sketch::SvgImportOptions options;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a = args[i];
    const bool has_value = i + 1 < args.size();
    if (a == "--fix") {
      options.fix = true;
    } else if (a == "--no-smooth") {
      options.smooth_joints = false;
    } else if (a == "--out" && has_value) {
      output = args[++i];
    } else if ((a == "--scale" || a == "--dpi" || a == "--gap") && has_value) {
      const auto v = number_arg(args[++i]);
      if (!v) {
        err << "vstudio-cli: " << a << " takes a number, not '" << args[i] << "'\n" << kUsage;
        return 2;
      }
      (a == "--scale" ? options.scale : a == "--dpi" ? options.pixels_per_inch : options.gap_mm) = *v;
    } else if (!a.starts_with("--") && !input) {
      input = a;
    } else {
      err << kUsage;
      return 2;
    }
  }
  if (!input) {
    err << kUsage;
    return 2;
  }

  const auto settings = core::ProductSettings::defaults();
  sketch::Sketch sk;
  sk.set_name(to_path(*input).stem().string());
  const auto imported = sketch::import_svg_file(to_path(*input), sk, options, settings);
  if (!imported) {
    err << imported.error().to_string() << "\n";
    return 1;
  }
  std::size_t kinds[5] = {};
  for (const auto& e : sk.entities()) {
    if (!e.construction) ++kinds[static_cast<std::size_t>(e.kind())];
  }
  out << fmt::format("imported {}: {} curves, {} constraints\n", *input,
                     imported->curves.size(), imported->constraints.size());
  out << fmt::format("  lines {}, arcs {}, circles {}, curves {}, construction {}\n",
                     kinds[static_cast<std::size_t>(sketch::EntityKind::Line)],
                     kinds[static_cast<std::size_t>(sketch::EntityKind::Arc)],
                     kinds[static_cast<std::size_t>(sketch::EntityKind::Circle)],
                     kinds[static_cast<std::size_t>(sketch::EntityKind::Bezier)],
                     imported->open.size());
  for (const auto& d : imported->diagnostics) out << "  " << d.to_string() << "\n";
  const auto report = sketch::solve(sk, settings);
  out << fmt::format("  status          {}\n", sketch::to_string(report.status));
  out << fmt::format("  dof             {}\n", report.dof);
  if (!report.ok()) {
    for (const auto& d : report.diagnostics) out << "  " << d.to_string() << "\n";
  }
  if (!report.ok() || !print_profiles(sk, settings, out, err)) return 1;

  if (output) {
    const std::string json = sketch::to_json(sk);
    if (!write_bytes(to_path(*output), std::as_bytes(std::span(json)), err)) return 1;
    out << "wrote " << *output << "\n";
  }
  return 0;
}

}  // namespace vs::cli

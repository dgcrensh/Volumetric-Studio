#include "draw_commands.hpp"

#include <charconv>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "config_commands.hpp"
#include "files.hpp"
#include "vs/draw/drawing.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/project.hpp"
#include "vs/proc/generator.hpp"

namespace vs::cli {

namespace {

std::string anchor_text(const model::Document& doc, const model::Anchor& a) {
  std::string target;
  switch (a.on) {
    case model::AnchorOn::Origin: return "origin";
    case model::AnchorOn::VolumeBoundary:
    case model::AnchorOn::VolumeContents: {
      const auto* v = doc.find_volume(a.target);
      target = fmt::format("{} {}", v ? v->name : std::string("?"),
                           a.on == model::AnchorOn::VolumeBoundary ? "boundary" : "contents");
      break;
    }
    case model::AnchorOn::Surface: {
      const auto* s = doc.find_connection(a.target);
      target = s ? s->name : std::string("?");
      break;
    }
  }
  switch (a.part) {
    case model::AnchorPart::Face: return fmt::format("{} face {}", target, a.face);
    case model::AnchorPart::Hole: return fmt::format("{} hole {}", target, a.hole + 1);
    default: return fmt::format("{} {}", target, model::to_string(a.part));
  }
}

}  // namespace

int run_drawing(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  constexpr std::string_view kUsage =
      "usage: vstudio-cli drawing <project.vsproj> [<name>] [--standard [<name>]] [--save]\n"
      "       [--svg <out.svg>] [--pdf <out.pdf>] [--dxf <out.dxf>] [--config <name>] "
      "[--threads N]\n";
  std::optional<std::string_view> project_arg, name, config;
  std::optional<std::string> standard;
  std::vector<std::filesystem::path> outputs;
  bool save = false;
  eval::EngineOptions engine_options;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a = args[i];
    if (a == "--standard") {
      standard = "Standard";
      if (i + 1 < args.size() && !args[i + 1].starts_with("-")) standard = std::string(args[++i]);
    } else if (a == "--save") {
      save = true;
    } else if ((a == "--svg" || a == "--pdf" || a == "--dxf") && i + 1 < args.size()) {
      auto p = to_path(args[++i]);
      if (p.extension().empty()) p += std::string(".") + std::string(a.substr(2));
      outputs.push_back(p);
    } else if (a == "--config" && i + 1 < args.size()) {
      config = args[++i];
    } else if (a == "--threads" && i + 1 < args.size()) {
      const std::string_view t = args[++i];
      std::size_t n = 0;
      const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), n);
      if (ec != std::errc{} || end != t.data() + t.size() || n == 0) return err << kUsage, 2;
      engine_options.threads = n;
    } else if (a.starts_with("-")) {
      return err << kUsage, 2;
    } else if (!project_arg) {
      project_arg = a;
    } else if (!name) {
      name = a;
    } else {
      return err << kUsage, 2;
    }
  }
  if (!project_arg || (save && !standard) || (standard && name)) return err << kUsage, 2;
  const std::filesystem::path path = to_path(*project_arg);
  auto project = model::load_project(path);
  if (!project) {
    err << project.error().to_string() << "\n";
    return 1;
  }
  if (config && !use_configuration(*project, *config, err)) return 1;

  const core::ProductSettings settings = core::ProductSettings::defaults();
  eval::Engine engine(settings, engine_options);
  proc::install(engine);
  const eval::Evaluation ev = engine.evaluate(*project);
  out << fmt::format("project     {}{}\n", ev.document.name,
                     ev.configuration_name.empty()
                         ? std::string{}
                         : fmt::format(" (configuration {})", ev.configuration_name));

  model::Drawing drawing;
  if (standard) {
    drawing = draw::standard_drawing(ev.document, ev.build, *standard);
  } else {
    const model::Drawing* found = name ? ev.document.find_drawing(*name)
                                  : ev.document.drawings.empty() ? nullptr
                                                                 : &ev.document.drawings.front();
    if (!found) {
      err << "vstudio-cli: "
          << (name ? fmt::format("no drawing named '{}'", *name)
                   : std::string("the project has no drawings (try --standard)"))
          << "\n";
      return 1;
    }
    drawing = *found;
  }

  draw::DrawOptions options;
  options.settings = settings;
  eval::ThreadPool& pool = engine.pool();
  options.parallel = [&pool](std::size_t n, const std::function<void(std::size_t)>& task) {
    pool.run(n, task);
  };
  const draw::DrawingResult r = draw::draw(ev, drawing, options);

  out << fmt::format("drawing     {}  {} x {} mm, {} view{}, {} dimension{}\n", drawing.name,
                     draw::number_text(drawing.width, 1), draw::number_text(drawing.height, 1),
                     drawing.views.size(), drawing.views.size() == 1 ? "" : "s",
                     drawing.dimensions.size(), drawing.dimensions.size() == 1 ? "" : "s");
  for (std::size_t i = 0; i < drawing.views.size(); ++i) {
    const auto& v = drawing.views[i];
    const auto& vr = r.views[i];
    out << fmt::format("  view      {:<12} {:<10} {:>6}  {} visible, {} hidden, {} outline lines\n",
                       v.name, model::to_string(v.direction), draw::scale_text(v.scale),
                       vr.visible_lines, vr.hidden_lines, vr.outline_lines);
  }
  for (std::size_t i = 0; i < drawing.dimensions.size(); ++i) {
    const auto& d = drawing.dimensions[i];
    const auto& dr = r.dimensions[i];
    const auto* view = drawing.find_view(d.view);
    std::string what = d.kind == model::DimensionKind::Diameter
                           ? anchor_text(ev.document, d.from)
                           : fmt::format("{} to {}", anchor_text(ev.document, d.from),
                                         anchor_text(ev.document, d.to));
    out << fmt::format("  dim {:<3}   {:<10} {:<10} {:<12} {:<10} {}\n", i + 1,
                       view ? view->name : std::string("?"), model::to_string(d.kind),
                       dr.drawn ? dr.text : std::string("-"),
                       dr.disposable ? "disposable" : "stable", what);
  }
  std::size_t errors = 0, warnings = 0;
  for (const auto& d : r.diagnostics) (d.is_error() ? errors : warnings)++;
  out << fmt::format("diagnostics {} error{}, {} warning{}\n", errors, errors == 1 ? "" : "s",
                     warnings, warnings == 1 ? "" : "s");
  for (const auto& d : r.diagnostics) out << "  " << d.to_string() << "\n";

  bool ok = r.ok();
  for (const auto& o : outputs) {
    const auto written = draw::write_sheet(r.sheet, o);
    for (const auto& d : written) out << "  " << d.to_string() << "\n";
    if (written.has_errors()) {
      ok = false;
      continue;
    }
    const auto u = o.generic_u8string();
    out << fmt::format("wrote       {} ({} bytes)\n", std::string(u.begin(), u.end()),
                       std::filesystem::file_size(o));
  }
  if (save) {
    model::Project saved = *model::load_project(path);
    saved.document.drawings.push_back(drawing);
    const auto layout = std::filesystem::is_directory(path) ? model::ProjectLayout::Folder
                                                            : model::ProjectLayout::Zip;
    const auto ds = model::save_project(saved, path, layout);
    for (const auto& d : ds) out << "  " << d.to_string() << "\n";
    if (ds.has_errors()) {
      ok = false;
    } else {
      out << "saved       drawing '" << drawing.name << "'\n";
    }
  }
  out << "result      " << (ok ? "ok" : "errors") << "\n";
  return ok ? 0 : 1;
}

}  // namespace vs::cli

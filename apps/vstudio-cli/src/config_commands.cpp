#include "config_commands.hpp"
#include "files.hpp"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "vs/core/settings.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/check.hpp"
#include "vs/model/configuration.hpp"
#include "vs/model/design_table.hpp"
#include "vs/proc/generator.hpp"

namespace vs::cli {

namespace {

model::ProjectLayout layout_of(const std::filesystem::path& path) {
  return std::filesystem::is_directory(path) ? model::ProjectLayout::Folder
                                             : model::ProjectLayout::Zip;
}

std::string scalar_text(const model::OutputValue& v) {
  if (const auto* d = std::get_if<double>(&v)) return fmt::format("{:.6g}", *d);
  if (const auto* p = std::get_if<core::dvec3>(&v)) {
    return fmt::format("({:.6g}, {:.6g}, {:.6g})", p->x, p->y, p->z);
  }
  const auto& b = std::get<core::dbox3>(v);
  return fmt::format("({:.6g}, {:.6g}, {:.6g})..({:.6g}, {:.6g}, {:.6g})", b.min.x, b.min.y,
                     b.min.z, b.max.x, b.max.y, b.max.z);
}

bool save(const model::Project& project, const std::filesystem::path& to,
          const std::filesystem::path& from, std::ostream& out) {
  const core::Diagnostics saved = model::save_project(project, to, layout_of(from));
  for (const auto& d : saved) out << "  " << d.to_string() << "\n";
  if (saved.has_errors()) return false;
  const auto u = to.generic_u8string();
  out << "saved       " << std::string(u.begin(), u.end()) << "\n";
  return true;
}

}  // namespace

bool use_configuration(model::Project& project, std::string_view name, std::ostream& err) {
  if (name == "-") {
    project.document.active_configuration = core::Id::null();
    return true;
  }
  const model::Configuration* c = project.document.find_configuration(name);
  if (c == nullptr) {
    err << "vstudio-cli: no configuration named '" << name << "'";
    if (!project.document.configurations.empty()) {
      err << " (";
      for (std::size_t k = 0; k < project.document.configurations.size(); ++k) {
        err << (k ? ", " : "") << "'" << project.document.configurations[k].name << "'";
      }
      err << ")";
    }
    err << "\n";
    return false;
  }
  project.document.active_configuration = c->id;
  return true;
}

int run_configs(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  constexpr std::string_view kUsage =
      "usage: vstudio-cli configs <project.vsproj> [--build] [--threads N] "
      "[--activate <name>|-]\n";
  std::optional<std::string_view> project_arg, activate;
  bool do_build = false;
  eval::EngineOptions engine_options;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a = args[i];
    if (a == "--build") {
      do_build = true;
    } else if (a == "--threads" && i + 1 < args.size()) {
      const std::string_view t = args[++i];
      std::size_t n = 0;
      const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), n);
      if (ec != std::errc{} || end != t.data() + t.size()) return err << kUsage, 2;
      engine_options.threads = n;
    } else if (a == "--activate" && i + 1 < args.size()) {
      activate = args[++i];
    } else if (a.starts_with("-") || project_arg) {
      return err << kUsage, 2;
    } else {
      project_arg = a;
    }
  }
  if (!project_arg) return err << kUsage, 2;
  const std::filesystem::path path = to_path(*project_arg);
  auto loaded = model::load_project(path);
  if (!loaded) {
    err << loaded.error().to_string() << "\n";
    return 1;
  }
  model::Document& doc = loaded->document;

  if (activate) {
    if (!use_configuration(*loaded, *activate, err)) return 1;
    return save(*loaded, path, path, out) ? 0 : 1;
  }

  const model::Configuration* active = doc.find_configuration(doc.active_configuration);
  out << fmt::format("project     {} ({} configuration{}, active: {})\n", doc.name,
                     doc.configurations.size(), doc.configurations.size() == 1 ? "" : "s",
                     active != nullptr ? active->name : "the authored values");
  for (const auto& c : doc.configurations) {
    out << fmt::format("  {}{}\n", c.name, c.id == doc.active_configuration ? "  (active)" : "");
    for (const auto& v : c.values) {
      const auto d = model::drivable(doc, v.target);
      out << fmt::format("    {} = {}{}\n", d ? d->label : std::string("(missing)"), v.expression,
                         d ? fmt::format("   (authored: {})", d->expression) : std::string{});
    }
  }
  if (!do_build) return 0;

  const auto settings = core::ProductSettings::defaults();
  eval::Engine engine(settings, engine_options);
  proc::install(engine);
  bool errors = false;
  std::vector<std::pair<std::string, core::Id>> runs{{"(authored)", core::Id::null()}};
  for (const auto& c : doc.configurations) runs.emplace_back(c.name, c.id);
  for (const auto& [name, id] : runs) {
    loaded->document.active_configuration = id;
    const auto t0 = std::chrono::steady_clock::now();
    const auto ev = engine.evaluate(*loaded);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::size_t current = 0;
    for (const auto& r : ev.volumes) {
      if (r.status == model::VolumeStatus::Current) ++current;
    }
    out << fmt::format("build       {}: {} of {} volumes current, {} regenerated, {} from cache, "
                       "{:.1f} ms, hash {}\n",
                       name, current, ev.volumes.size(), ev.stats.regenerated,
                       ev.stats.cache_hits, ms, ev.output_hash().to_hex().substr(0, 16));
    for (std::size_t i = 0; i < ev.volumes.size(); ++i) {
      const auto& r = ev.volumes[i];
      const model::Volume& v = ev.document.volumes[i];
      if (!r.contents || v.generated) continue;
      for (const auto& o : r.contents->outputs) {
        const model::PublishedOutput* po = v.find_output(o.output);
        if (po == nullptr || !o.value) continue;
        out << fmt::format("    {}.{} = {}\n", v.name, po->name, scalar_text(*o.value));
      }
    }
    for (const auto& d : ev.diagnostics) {
      if (d.is_error()) out << "  " << d.to_string() << "\n";
    }
    errors = errors || !ev.ok();
  }
  out << "result      " << (errors ? "errors" : "ok") << "\n";
  return errors ? 1 : 0;
}

int run_design_table(std::span<const std::string_view> args, std::ostream& out,
                     std::ostream& err) {
  constexpr std::string_view kUsage =
      "usage: vstudio-cli design-table <project.vsproj> [--all] [--out <table.csv>]\n"
      "       vstudio-cli design-table <project.vsproj> --import <table.csv> "
      "[--out <project>]\n";
  std::optional<std::string_view> project_arg, import, out_arg;
  bool all = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a = args[i];
    if (a == "--all") {
      all = true;
    } else if (a == "--import" && i + 1 < args.size()) {
      import = args[++i];
    } else if (a == "--out" && i + 1 < args.size()) {
      out_arg = args[++i];
    } else if (a.starts_with("-") || project_arg) {
      return err << kUsage, 2;
    } else {
      project_arg = a;
    }
  }
  if (!project_arg || (import && all)) return err << kUsage, 2;
  const std::filesystem::path path = to_path(*project_arg);
  auto loaded = model::load_project(path);
  if (!loaded) {
    err << loaded.error().to_string() << "\n";
    return 1;
  }

  if (!import) {
    const std::string csv = model::to_csv(model::design_table(loaded->document, all));
    if (!out_arg) {
      out << csv;
      return 0;
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(csv.data());
    return write_bytes(to_path(*out_arg), std::span(bytes, csv.size()), err) ? 0 : 1;
  }

  std::ifstream f(to_path(*import), std::ios::binary);
  if (!f) return err << "vstudio-cli: can't read " << *import << "\n", 1;
  std::ostringstream text;
  text << f.rdbuf();
  const auto table = model::from_csv(loaded->document, text.str());
  if (!table) return err << table.error().to_string() << "\n", 1;
  const auto applied = model::apply_design_table(loaded->document, *table);
  if (!applied) return err << applied.error().to_string() << "\n", 1;
  const core::Diagnostics problems =
      model::edit(loaded->document, [&](model::Document& d) { d = *applied; });
  for (const auto& d : problems) out << d.to_string() << "\n";
  if (problems.has_errors()) return 1;
  out << fmt::format("table       {} configuration{}, {} column{}\n", table->rows.size(),
                     table->rows.size() == 1 ? "" : "s", table->columns.size(),
                     table->columns.size() == 1 ? "" : "s");
  return save(*loaded, out_arg ? to_path(*out_arg) : path, path, out) ? 0 : 1;
}

}  // namespace vs::cli

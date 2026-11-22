#include "eval_commands.hpp"
#include "files.hpp"
#include "config_commands.hpp"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "vs/core/settings.hpp"
#include "vs/eval/engine.hpp"
#include "vs/eval/step_export.hpp"
#include "vs/mesh/combine.hpp"
#include "vs/mesh/io.hpp"
#include "vs/model/project.hpp"
#include "vs/proc/generator.hpp"

namespace vs::cli {

namespace {

std::string value_text(const model::OutputValue& v) {
  if (const auto* d = std::get_if<double>(&v)) return fmt::format("{:.6g}", *d);
  if (const auto* p = std::get_if<core::dvec3>(&v)) {
    return fmt::format("({:.6g}, {:.6g}, {:.6g})", p->x, p->y, p->z);
  }
  const auto& b = std::get<core::dbox3>(v);
  return fmt::format("({:.6g}, {:.6g}, {:.6g})..({:.6g}, {:.6g}, {:.6g})", b.min.x, b.min.y,
                     b.min.z, b.max.x, b.max.y, b.max.z);
}

std::string how(const eval::VolumeResult& r) {
  if (r.regenerated) return "regenerated";
  if (r.from_disk) return "disk cache";
  if (r.from_cache) return "cache";
  return "not run";
}

void print(const eval::Evaluation& ev, double ms, std::ostream& out) {
  for (std::size_t i = 0; i < ev.volumes.size(); ++i) {
    const auto& r = ev.volumes[i];
    const model::Volume& v = ev.document.volumes[i];
    out << fmt::format("  {:<14} {:<9} {:<12} key {}{}\n", v.name, model::to_string(r.status),
                       how(r), r.key ? r.key->to_hex().substr(0, 12) : std::string("-"),
                       r.stale_contents ? "  (showing stale contents)" : "");
    if (const proc::FieldContents* fc = proc::field_contents(r)) {
      const auto& g = fc->samples.grid;
      out << fmt::format("    field {} x {} x {} samples, {} mm apart\n", g.nx, g.ny, g.nz, g.cell);
      if (const mesh::Mesh* m = proc::contents_mesh(r)) {
        out << fmt::format("    mesh {} triangles, {} vertices, {} seam{} stitched\n",
                           m->triangle_count(), m->vertex_count(), fc->stitched.size(),
                           fc->stitched.size() == 1 ? "" : "s");
      }
    }
    if (r.contents) {
      for (const auto& o : r.contents->outputs) {
        const model::PublishedOutput* po = v.find_output(o.output);
        if (po == nullptr || !o.value) continue;
        out << fmt::format("    {} = {}\n", po->name, value_text(*o.value));
      }
    }
  }
  for (const auto& d : ev.diagnostics) {
    if (d.severity != core::Severity::Info) out << "  " << d.to_string() << "\n";
  }
  const auto& s = ev.stats;
  out << fmt::format(
      "stats       {} regenerated, {} from cache ({} from disk), {} blocked, {} waves, {} layout "
      "builds ({} reused), {:.1f} ms\n",
      s.regenerated, s.cache_hits, s.disk_hits, s.blocked, s.waves, s.layout_builds,
      s.layouts_reused, ms);
  if (ev.combined) {
    const auto& w = ev.combined->weld;
    out << fmt::format("weld        {} triangles, {} shell{}, watertight {}, manifold {}\n",
                       w.mesh.triangle_count(), w.topology.shells,
                       w.topology.shells == 1 ? "" : "s", w.topology.watertight() ? "yes" : "no",
                       w.topology.manifold() ? "yes" : "no");
  }
  out << "hash        " << ev.output_hash().to_hex() << "\n";
}

}  // namespace

int run_rebuild(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  constexpr std::string_view kUsage =
      "usage: vstudio-cli rebuild <project.vsproj> [--threads N] [--check-determinism] [--weld] "
      "[--stl <out.stl>] [--3mf <out.3mf> [--per-volume]] [--step <out.step> "
      "[--procedural-boundary]] [--cache] "
      "[--set <volume>.<parameter>=<value>]... [--config <name>] [--twice]\n";
  std::optional<std::string_view> project_arg;
  eval::EngineOptions engine_options;
  eval::EvaluateOptions options;
  bool cache = false, twice = false;
  std::optional<std::string_view> config;
  std::optional<std::string_view> stl, three_mf;
  bool per_volume = false;
  std::optional<std::string_view> step;
  eval::StepExportOptions step_options;
  std::vector<std::string_view> sets;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto a = args[i];
    if (a == "--threads" && i + 1 < args.size()) {
      const auto t = args[++i];
      std::size_t n = 0;
      const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), n);
      if (ec != std::errc() || p != t.data() + t.size() || n == 0 || n > 256) {
        return err << "vstudio-cli: --threads takes a number from 1 to 256\n", 2;
      }
      engine_options.threads = n;
    } else if (a == "--check-determinism") {
      engine_options.check_determinism = true;
    } else if (a == "--weld") {
      options.weld = true;
    } else if (a == "--stl" && i + 1 < args.size()) {
      stl = args[++i];
      options.weld = true;
    } else if (a == "--3mf" && i + 1 < args.size()) {
      three_mf = args[++i];
      options.weld = true;
    } else if (a == "--per-volume") {
      per_volume = true;
    } else if (a == "--step" && i + 1 < args.size()) {
      step = args[++i];
    } else if (a == "--procedural-boundary") {
      step_options.procedural = eval::ProceduralAs::Boundary;
    } else if (a == "--cache") {
      cache = true;
    } else if (a == "--twice") {
      twice = true;
    } else if (a == "--set" && i + 1 < args.size()) {
      sets.push_back(args[++i]);
    } else if (a == "--config" && i + 1 < args.size()) {
      config = args[++i];
    } else if (a.starts_with("-") || project_arg) {
      return err << kUsage, 2;
    } else {
      project_arg = a;
    }
  }
  if (!project_arg || (per_volume && !three_mf)) return err << kUsage, 2;

  const std::filesystem::path path = to_path(*project_arg);
  auto loaded = model::load_project(path);
  if (!loaded) {
    err << loaded.error().to_string() << "\n";
    return 1;
  }
  // What a zip project's caches are saved with: the project as it was
  // read, never with --set's changes.
  const bool zip = !std::filesystem::is_directory(path);
  std::optional<model::Project> as_read;
  if (cache && zip) as_read = *loaded;
  if (config && !use_configuration(*loaded, *config, err)) return 1;
  for (const auto set : sets) {
    const auto dot = set.find('.');
    const auto eq = set.find('=');
    if (dot == std::string_view::npos || eq == std::string_view::npos || eq < dot) {
      return err << "vstudio-cli: --set takes <volume>.<parameter>=<value>\n", 2;
    }
    model::Volume* v = nullptr;
    for (auto& candidate : loaded->document.volumes) {
      if (candidate.name == set.substr(0, dot)) v = &candidate;
    }
    const std::string name(set.substr(dot + 1, eq - dot - 1));
    model::VolumeParameter* p = nullptr;
    if (v != nullptr) {
      for (auto& candidate : v->parameters) {
        if (candidate.name == name) p = &candidate;
      }
    }
    if (p == nullptr) {
      return err << "vstudio-cli: no parameter '" << set.substr(0, eq) << "'\n", 1;
    }
    p->value = feature::Param::expr(std::string(set.substr(eq + 1)));
  }
  // A folder project's cache/ is the engine's disk cache; a zip's cache/
  // entries are read into memory now and written back after (H4).
  if (cache && !zip) engine_options.disk_cache = path / "cache";

  const auto settings = core::ProductSettings::defaults();
  eval::Engine engine(settings, engine_options);
  proc::install(engine);  // procedural volumes (M10)
  if (cache && zip) {
    const model::CacheFiles files = model::load_project_cache(path);
    const std::size_t entries = engine.cache().insert_files(files);
    out << fmt::format("cache       {} entr{} read from the project\n", entries,
                       entries == 1 ? "y" : "ies");
  }
  const std::size_t volumes = loaded->document.volumes.size();
  out << fmt::format("project     {} ({} volume{}, {} thread{}{})\n", loaded->document.name,
                     volumes, volumes == 1 ? "" : "s", engine.pool().size(),
                     engine.pool().size() == 1 ? "" : "s",
                     engine_options.check_determinism ? ", checking determinism" : "");
  if (const auto* c = loaded->document.find_configuration(loaded->document.active_configuration)) {
    out << fmt::format("configuration {}\n", c->name);
  }
  bool errors = false;
  for (int run = 0; run < (twice ? 2 : 1); ++run) {
    if (twice) out << fmt::format("evaluation  {}\n", run + 1);
    const auto t0 = std::chrono::steady_clock::now();
    const auto ev = engine.evaluate(*loaded, options);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    print(ev, ms, out);
    errors = errors || !ev.ok();
    if (stl && run + 1 == (twice ? 2 : 1) && !errors && ev.combined && ev.combined->ok()) {
      const auto bytes = mesh::write_stl(ev.combined->weld.mesh);
      if (!bytes) return err << bytes.error().to_string() << "\n", 1;
      if (!write_bytes(to_path(*stl), *bytes, err)) return 1;
      out << fmt::format("wrote       {} (STL, {} bytes)\n", *stl, bytes->size());
    }
    if (three_mf && run + 1 == (twice ? 2 : 1) && !errors && ev.combined && ev.combined->ok()) {
      // One object welded from every volume, or one per volume (each as
      // the engine meshed it: tessellated, generated or read).
      std::vector<mesh::ThreeMfObject> objects;
      if (per_volume) {
        for (const mesh::VolumeTessellation& v : ev.combined->volumes) {
          objects.push_back({v.name, v.volume, &v.mesh});
        }
      } else {
        objects.push_back({ev.document.name, core::Id::null(), &ev.combined->weld.mesh});
      }
      const auto bytes = mesh::write_3mf(objects, ev.document.name);
      if (!bytes) return err << bytes.error().to_string() << "\n", 1;
      if (!write_bytes(to_path(*three_mf), *bytes, err)) return 1;
      out << fmt::format("wrote       {} (3MF, {} object{}, {} bytes)\n", *three_mf, objects.size(),
                         objects.size() == 1 ? "" : "s", bytes->size());
    }
    if (as_read && run + 1 == (twice ? 2 : 1)) {
      const model::CacheFiles files = eval::project_cache_files(ev);
      const core::Diagnostics saved =
          model::save_project(*as_read, path, model::ProjectLayout::Zip, &files);
      for (const auto& d : saved) out << "  " << d.to_string() << "\n";
      if (saved.has_errors()) return 1;
      out << fmt::format("cache       {} file{} saved with the project\n", files.size(),
                         files.size() == 1 ? "" : "s");
    }
    if (step && run + 1 == (twice ? 2 : 1) && !errors) {
      const auto r = eval::export_step(ev, to_path(*step), step_options);
      for (const auto& d : r.diagnostics) out << "  " << d.to_string() << "\n";
      if (!r.ok()) return 1;
      out << fmt::format("wrote       {} (STEP, {} parts: {} B-rep, {} mesh, {} boundary)\n", *step,
                         r.parts.size(), r.breps, r.meshes, r.boundaries);
    }
  }
  out << "result      " << (errors ? "errors" : "ok") << "\n";
  return errors ? 1 : 0;
}

}  // namespace vs::cli

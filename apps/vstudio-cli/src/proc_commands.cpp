#include "proc_commands.hpp"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <optional>
#include <fstream>
#include <sstream>
#include <string>

#include <fmt/format.h>

#include "vs/core/settings.hpp"
#include "vs/eval/engine.hpp"
#include "vs/feature/tree.hpp"
#include "vs/mesh/io.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"
#include "vs/model/project.hpp"
#include "vs/proc/bake.hpp"
#include "vs/proc/check.hpp"
#include "vs/proc/evaluate.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/graph.hpp"
#include "vs/proc/inputs.hpp"
#include "vs/proc/inspect.hpp"
#include "vs/proc/mesher.hpp"
#include "vs/proc/nodes.hpp"
#include "vs/proc/templates.hpp"
#include "vs/validate/containment.hpp"

namespace vs::cli {

namespace {

std::string setting_text(const proc::Setting& s) {
  if (const auto* d = std::get_if<double>(&s)) return fmt::format("{}", *d);
  if (const auto* v = std::get_if<core::dvec3>(&s)) return fmt::format("({}, {}, {})", v->x, v->y, v->z);
  return fmt::format("'{}'", std::get<std::string>(s));
}

std::string ports(const std::vector<proc::PortSpec>& list) {
  std::string out;
  for (const auto& p : list) {
    if (!out.empty()) out += ", ";
    out += fmt::format("{}: {}", p.name, proc::to_string(p.type));
    if (p.default_value) out += " = " + setting_text(*p.default_value);
  }
  return out.empty() ? "-" : out;
}

std::filesystem::path to_path(std::string_view arg) {
  return std::filesystem::path(std::u8string(arg.begin(), arg.end()));
}

std::optional<double> number(std::string_view text) {
  double v = 0;
  const auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc() || p != text.data() + text.size()) return std::nullopt;
  return v;
}

double ms_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Finds a volume by name (or id) in a loaded project.
const model::Volume* volume_named(const model::Project& p, std::string_view name) {
  if (const model::Volume* v = p.document.find_volume(name)) return v;
  if (const auto id = core::Id::parse(name)) return p.document.find_volume(*id);
  return nullptr;
}

int save(const model::Project& project, const std::filesystem::path& from,
         const std::filesystem::path& to, std::ostream& out) {
  const auto layout = std::filesystem::is_directory(from) ? model::ProjectLayout::Folder
                                                          : model::ProjectLayout::Zip;
  const core::Diagnostics saved = model::save_project(project, to, layout);
  for (const auto& d : saved) out << "  " << d.to_string() << "\n";
  if (saved.has_errors()) return 1;
  out << "saved       " << to.string() << "\n";
  return 0;
}

}  // namespace

int run_nodes(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  if (!args.empty()) return err << "usage: vstudio-cli nodes\n", 2;
  const auto types = proc::library().types();
  out << fmt::format("node types  {}\n", types.size());
  for (const proc::NodeType* t : types) {
    out << fmt::format("  {:<28} {}\n", t->type, t->doc);
    out << fmt::format("    in  {}\n    out {}\n", ports(t->inputs), ports(t->outputs));
    for (const auto& p : t->properties) {
      out << fmt::format("    property {}{}\n", p.name,
                         p.default_value ? " = " + setting_text(*p.default_value) : std::string());
    }
  }
  return 0;
}

int run_graph(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  if (args.size() != 1) return err << "usage: vstudio-cli graph <graph.json>\n", 2;
  const std::filesystem::path path(std::u8string(args[0].begin(), args[0].end()));
  std::ifstream in(path, std::ios::binary);
  if (!in) return err << "vstudio-cli: can't read " << args[0] << "\n", 1;
  std::stringstream text;
  text << in.rdbuf();
  const auto graph = proc::graph_from_json(text.str());
  if (!graph) {
    out << "  " << graph.error().to_string() << "\nresult      errors\n";
    return 1;
  }
  out << fmt::format("graph       {} nodes, {} edges, {} parameters\n", graph->nodes.size(),
                     graph->edges.size(), graph->parameters.size());
  for (const auto& p : graph->parameters) out << fmt::format("  parameter {} = {}\n", p.name, p.value);
  const auto diagnostics = proc::check_graph(*graph);
  for (const std::size_t i : proc::topological_order(*graph)) {
    const proc::Node& n = graph->nodes[i];
    out << fmt::format("  {:<16} {}\n", proc::node_subject(n).label, n.type);
    for (const auto& e : graph->edges) {
      if (e.to.node != n.id) continue;
      const proc::Node* from = graph->find_node(e.from.node);
      out << fmt::format("    {} <- {}.{}\n", e.to.port,
                         from != nullptr ? proc::node_subject(*from).label : std::string("?"),
                         e.from.port);
    }
    for (const auto& [k, v] : n.settings) out << fmt::format("    {} = {}\n", k, setting_text(v));
  }
  for (const auto& d : diagnostics) out << "  " << d.to_string() << "\n";
  out << "result      " << (diagnostics.has_errors() ? "errors" : "ok") << "\n";
  return diagnostics.has_errors() ? 1 : 0;
}

int run_templates(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  constexpr std::string_view kUsage =
      "usage: vstudio-cli templates [<name> [--surface <id>] [--cell <mm>]]\n";
  if (args.empty()) {
    for (const auto& t : proc::templates()) {
      out << fmt::format("  {:<16} {}{}\n", t.name, t.doc,
                         t.needs_surface ? " (needs --surface)" : "");
    }
    return 0;
  }
  proc::TemplateOptions o;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--surface" && i + 1 < args.size()) {
      o.surface = core::Id::parse(args[++i]);
      if (!o.surface) return err << "vstudio-cli: --surface needs a 32-digit id\n", 2;
    } else if (args[i] == "--cell" && i + 1 < args.size()) {
      const auto c = number(args[++i]);
      if (!c || !(*c > 0)) return err << kUsage, 2;
      o.cell_size = *c;
    } else {
      return err << kUsage, 2;
    }
  }
  auto graph = proc::make_template(args[0], o);
  if (!graph) {
    err << graph.error().to_string() << "\n";
    return 1;
  }
  out << proc::to_json(*graph) << "\n";
  return 0;
}

int run_bench_gyroid(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  constexpr std::string_view kUsage =
      "usage: vstudio-cli bench-gyroid [--size <mm>] [--cell <mm>] [--threads N] [--3mf <file>]\n";
  double size = 100.0;
  double cell = 0.5;
  std::size_t threads = 0;
  std::optional<std::string_view> three_mf;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const bool has_value = i + 1 < args.size();
    if (args[i] == "--size" && has_value) {
      const auto v = number(args[++i]);
      if (!v || !(*v > 0)) return err << kUsage, 2;
      size = *v;
    } else if (args[i] == "--cell" && has_value) {
      const auto v = number(args[++i]);
      if (!v || !(*v > 0)) return err << kUsage, 2;
      cell = *v;
    } else if (args[i] == "--threads" && has_value) {
      const auto v = number(args[++i]);
      if (!v || !(*v >= 0)) return err << kUsage, 2;
      threads = static_cast<std::size_t>(*v);
    } else if (args[i] == "--3mf" && has_value) {
      three_mf = args[++i];
    } else {
      return err << kUsage, 2;
    }
  }
  const auto settings = core::ProductSettings::defaults();

  // One procedural cube, filled by the gyroid template.
  model::Project project;
  model::Document& doc = project.document;
  doc = model::make_document("Gyroid bench");
  using feature::Param;
  const auto point = [&](const std::string& name, double v) {
    return doc.layout.add(
        feature::DatumPointFeature{feature::PointAt{Param::of(v), Param::of(v), Param::of(v)}}, name);
  };
  model::Volume v;
  v.id = core::Id::random();
  v.name = "Cube";
  v.boundary = model::BoxBoundary{feature::FeatureRef{point("min", 0)},
                                  feature::FeatureRef{point("max", size)}};
  proc::TemplateOptions o;
  o.cell_size = cell;
  v.contents = model::ProceduralContents{proc::to_json(*proc::make_template("gyroid_infill", o))};
  v.outputs.push_back({core::Id::random(), "mass", model::OutputQuantity::Mass, 1.24});
  doc.volumes.push_back(v);

  eval::EngineOptions eo;
  eo.threads = threads;
  eval::Engine engine(settings, eo);
  proc::install(engine);
  eval::ThreadPool pool(threads);
  out << fmt::format("gyroid      {} mm cube, {} mm grid, wall 1.2, cell 8, sheet 0.8, {} threads\n",
                     size, cell, pool.size());

  // The stages one by one, as the generator runs them.
  auto t = std::chrono::steady_clock::now();
  const model::DocumentBuild layout = model::build_layout(doc, settings);
  auto inputs = proc::gather_inputs(doc, layout, 0, settings);
  if (!inputs) {
    err << inputs.error().to_string() << "\n";
    return 1;
  }
  out << fmt::format("  inputs        {:9.1f} ms  (layout, boundary tessellation)\n", ms_since(t));
  t = std::chrono::steady_clock::now();
  const auto graph = proc::graph_from_json(std::get<model::ProceduralContents>(v.contents).graph);
  const proc::GraphEvaluation ev = proc::evaluate_graph(*graph, *inputs);
  auto grid = proc::grid_covering(inputs->bounds, ev.cell_size);
  if (!ev.ok() || !grid) {
    err << "vstudio-cli: the graph didn't evaluate\n";
    return 1;
  }
  const proc::SampledField samples = proc::sample(**ev.contents, *grid, &pool);
  out << fmt::format("  sample        {:9.1f} ms  ({} x {} x {} = {} samples)\n", ms_since(t),
                     grid->nx, grid->ny, grid->nz, grid->size());
  t = std::chrono::steady_clock::now();
  const proc::SampledField clipped =
      proc::clip_to_boundary(samples, *inputs->boundary, proc::kContentsInset, &pool);
  out << fmt::format("  clip          {:9.1f} ms\n", ms_since(t));
  t = std::chrono::steady_clock::now();
  proc::UnmergedNet unmerged = proc::unmerged_net(clipped, &pool);
  out << fmt::format("  unmerged net  {:9.1f} ms  ({} triangles)\n", ms_since(t),
                     unmerged.triangles.size());
  t = std::chrono::steady_clock::now();
  const proc::FieldMesh nets = proc::merge_net(std::move(unmerged), clipped.grid, &pool);
  out << fmt::format("  merge         {:9.1f} ms  ({} triangles)\n", ms_since(t),
                     nets.triangles.size());
  t = std::chrono::steady_clock::now();
  const proc::ContentsMesh cm = proc::mesh_contents(samples, *inputs->boundary, {},
                                                    doc.volume_subject(v.id), &pool);
  out << fmt::format("  mesh contents {:9.1f} ms  (clip, nets, seams, containment moves; {} "
                     "triangles, {} vertices moved)\n",
                     ms_since(t), cm.mesh.triangle_count(), cm.moved_vertices);
  t = std::chrono::steady_clock::now();
  const auto contained = validate::check_containment(
      layout.volumes[0].boundary, validate::MeshView{cm.mesh.positions, cm.mesh.indices, {}},
      doc.volume_subject(v.id), settings,
      [&pool](std::size_t n, const std::function<void(std::size_t)>& task) { pool.run(n, task); });
  out << fmt::format("  containment   {:9.1f} ms  ({})\n", ms_since(t),
                     contained.contained ? "contained" : "OUTSIDE");

  // And the whole generation through the engine, as a project rebuild runs it.
  t = std::chrono::steady_clock::now();
  const auto result = engine.evaluate(project);
  const double total = ms_since(t);
  for (const auto& d : result.diagnostics) {
    if (d.severity != core::Severity::Info) out << "  " << d.to_string() << "\n";
  }
  const mesh::Mesh* m = proc::contents_mesh(result.volumes[0]);
  if (!result.ok() || m == nullptr) {
    out << "result      failed\n";
    return 1;
  }
  out << fmt::format("engine      {:.1f} ms total: {} triangles, {} vertices\n", total,
                     m->triangle_count(), m->vertex_count());
  if (const auto& c = result.volumes[0].contents; c && !c->outputs.empty() && c->outputs[0].value) {
    out << fmt::format("mass        {:.1f} g at 1.24 g/cm³\n", std::get<double>(*c->outputs[0].value));
  }
  if (three_mf) {
    const mesh::ThreeMfObject object{"Gyroid", v.id, m};
    auto bytes = mesh::write_3mf(std::span(&object, 1), "Gyroid bench");
    if (!bytes) {
      err << bytes.error().to_string() << "\n";
      return 1;
    }
    std::ofstream f(to_path(*three_mf), std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    out << "wrote       " << *three_mf << "\n";
  }
  return 0;
}

int run_inspect(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  constexpr std::string_view kUsage =
      "usage: vstudio-cli inspect <project.vsproj> <volume> <node> [--preview]\n";
  std::vector<std::string_view> positional;
  bool want_preview = false;
  for (const auto a : args) {
    if (a == "--preview") {
      want_preview = true;
    } else if (a.starts_with("-")) {
      return err << kUsage, 2;
    } else {
      positional.push_back(a);
    }
  }
  if (positional.size() != 3) return err << kUsage, 2;
  auto loaded = model::load_project(to_path(positional[0]));
  if (!loaded) return err << loaded.error().to_string() << "\n", 1;
  const model::Volume* v = volume_named(*loaded, positional[1]);
  if (v == nullptr) return err << "vstudio-cli: no volume '" << positional[1] << "'\n", 1;
  const auto* contents = std::get_if<model::ProceduralContents>(&v->contents);
  if (contents == nullptr) return err << "vstudio-cli: '" << v->name << "' isn't procedural\n", 1;
  const auto graph = proc::graph_from_json(contents->graph);
  if (!graph) return err << graph.error().to_string() << "\n", 1;
  const proc::Node* node = graph->find_node(positional[2]);
  if (node == nullptr) {
    if (const auto id = core::Id::parse(positional[2])) node = graph->find_node(*id);
  }
  if (node == nullptr) return err << "vstudio-cli: no node '" << positional[2] << "'\n", 1;

  const auto settings = core::ProductSettings::defaults();
  const model::DocumentBuild layout = model::build_layout(loaded->document, settings);
  std::size_t index = 0;
  while (loaded->document.volumes[index].id != v->id) ++index;
  eval::ThreadPool pool;
  proc::EvaluateGraphOptions options;
  options.pool = &pool;
  const auto t = std::chrono::steady_clock::now();
  const proc::NodeInspection ins =
      proc::inspect_volume_node(loaded->document, layout, index, settings, node->id, options);
  out << fmt::format("node        {} ({}), {} nodes evaluated, {:.1f} ms\n",
                     proc::node_subject(*node).label, node->type, ins.evaluated, ms_since(t));
  for (const auto& d : ins.diagnostics) out << "  " << d.to_string() << "\n";
  if (!ins.ok()) return 1;
  auto inputs = proc::gather_inputs(loaded->document, layout, index, settings);
  for (const auto& [port, value] : ins.outputs) {
    out << fmt::format("  {:<12} {}\n", port, proc::describe(value));
    if (!want_preview || !inputs) continue;
    const proc::Preview p = proc::preview(value, *inputs, 0.0, &pool);
    if (p.mesh) {
      out << fmt::format("    preview mesh {} triangles\n", p.mesh->triangle_count());
    }
    if (!p.points.empty()) out << fmt::format("    preview {} points\n", p.points.size());
  }
  return 0;
}

namespace {

int bake_or_unbake(std::span<const std::string_view> args, std::ostream& out, std::ostream& err,
                   bool baking) {
  const std::string usage = fmt::format("usage: vstudio-cli {} <project.vsproj> <volume> [--out <project>]\n",
                                        baking ? "bake" : "unbake");
  std::vector<std::string_view> positional;
  std::optional<std::string_view> out_path;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--out" && i + 1 < args.size()) {
      out_path = args[++i];
    } else if (args[i].starts_with("-")) {
      return err << usage, 2;
    } else {
      positional.push_back(args[i]);
    }
  }
  if (positional.size() != 2) return err << usage, 2;
  const std::filesystem::path path = to_path(positional[0]);
  auto loaded = model::load_project(path);
  if (!loaded) return err << loaded.error().to_string() << "\n", 1;
  const model::Volume* v = volume_named(*loaded, positional[1]);
  if (v == nullptr) return err << "vstudio-cli: no volume '" << positional[1] << "'\n", 1;
  const core::Id id = v->id;
  core::Diagnostics result;
  if (baking) {
    eval::Engine engine(core::ProductSettings::defaults());
    proc::install(engine);
    const auto ev = engine.evaluate(*loaded);
    const eval::VolumeResult* r = ev.find(id);
    const mesh::Mesh* m = r != nullptr ? proc::contents_mesh(*r) : nullptr;
    if (r == nullptr || r->status != model::VolumeStatus::Current || m == nullptr) {
      for (const auto& d : ev.diagnostics) out << "  " << d.to_string() << "\n";
      return err << "vstudio-cli: '" << v->name << "' didn't generate, so there's nothing to bake\n", 1;
    }
    out << fmt::format("generated   {} triangles\n", m->triangle_count());
    result = proc::bake(*loaded, id, *m);
  } else {
    result = proc::unbake(*loaded, id);
  }
  for (const auto& d : result) out << "  " << d.to_string() << "\n";
  if (result.has_errors()) return 1;
  out << fmt::format("{}       content hash {}\n", baking ? "baked " : "unbaked",
                     model::content_hash(*loaded).to_hex().substr(0, 16));
  return save(*loaded, path, out_path ? to_path(*out_path) : path, out);
}

}  // namespace

int run_bake(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  return bake_or_unbake(args, out, err, true);
}

int run_unbake(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  return bake_or_unbake(args, out, err, false);
}

}  // namespace vs::cli

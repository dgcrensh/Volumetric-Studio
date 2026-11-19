#include "model_commands.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "vs/kernel/queries.hpp"
#include "vs/model/build.hpp"
#include "vs/model/dependency.hpp"
#include "vs/model/project.hpp"

namespace vs::cli {

namespace {

using namespace vs::model;

std::filesystem::path to_path(std::string_view arg) {
  return std::filesystem::path(std::u8string(arg.begin(), arg.end()));
}

std::string param_text(const Param& p) {
  if (!p.expression.empty()) return p.expression;
  return fmt::format("{:g}", p.value);
}

std::string join(const std::vector<std::string>& items, std::string_view sep = ", ") {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) out += sep;
    out += items[i];
  }
  return out;
}

std::string boundary_text(const Document& doc, const Volume& v) {
  if (const auto* fb = std::get_if<FeatureBoundary>(&v.boundary)) {
    std::vector<std::string> names;
    for (const auto& id : fb->features) names.push_back(doc.layout.label(id));
    return "features " + join(names);
  }
  return "box between two points";
}

std::string contents_text(const Volume& v) {
  if (v.keep_out) return "keep-out (stays empty)";
  if (v.contents_empty()) return "placeholder";
  if (const auto* s = std::get_if<StaticContents>(&v.contents)) {
    return fmt::format("file {} ({})", s->blob, s->format);
  }
  if (const auto* t = v.tree()) {
    return fmt::format("{} features, {} variables", t->features().size(),
                       t->variables().variables().size());
  }
  return fmt::format("node graph, {} bytes", std::get<ProceduralContents>(v.contents).graph.size());
}

std::string geometry_text(const Document& doc, const ConnectionSurface& c) {
  if (const auto* sp = std::get_if<SketchPatch>(&c.geometry)) {
    return fmt::format("sketch '{}'", doc.layout.label(sp->sketch));
  }
  const auto& dp = std::get<DatumPatch>(c.geometry);
  return fmt::format("{} x {} rectangle on a plane", param_text(dp.width), param_text(dp.height));
}

std::string params_text(const ConnectionSurface& c) {
  if (const auto* cl = std::get_if<ClearanceParams>(&c.params)) {
    return fmt::format("gap {}", param_text(cl->gap));
  }
  if (const auto* b = std::get_if<BoltedFlangeParams>(&c.params)) {
    std::string holes;
    if (const auto* bc = std::get_if<BoltCircle>(&b->holes.layout)) {
      holes = fmt::format("{} holes on a {} circle", param_text(bc->count),
                          param_text(bc->diameter));
    } else {
      const auto& g = std::get<BoltGrid>(b->holes.layout);
      holes = fmt::format("{} x {} grid at {} x {}", param_text(g.columns), param_text(g.rows),
                          param_text(g.pitch_x), param_text(g.pitch_y));
    }
    return fmt::format("{} bolts, {}, min wall {}", b->thread, holes, param_text(b->min_wall));
  }
  return "no parameters";
}

std::string value_text(const OutputValue& v) {
  if (const auto* d = std::get_if<double>(&v)) return fmt::format("{:.6g}", *d);
  if (const auto* p = std::get_if<core::dvec3>(&v)) {
    return fmt::format("({:.6g}, {:.6g}, {:.6g})", p->x, p->y, p->z);
  }
  const auto& b = std::get<core::dbox3>(v);
  return fmt::format("({:.6g}, {:.6g}, {:.6g}) to ({:.6g}, {:.6g}, {:.6g})", b.min.x, b.min.y,
                     b.min.z, b.max.x, b.max.y, b.max.z);
}

}  // namespace

int run_info(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  bool do_build = false;
  std::vector<std::string_view> paths;
  for (const auto a : args) {
    if (a == "--build") {
      do_build = true;
    } else {
      paths.push_back(a);
    }
  }
  if (paths.size() != 1) {
    err << "usage: vstudio-cli info <project.vsproj> [--build]\n";
    return 2;
  }
  const auto path = to_path(paths[0]);
  auto loaded = load_project(path);
  if (!loaded) {
    err << loaded.error().to_string() << "\n";
    return 1;
  }
  const Project& project = *loaded;
  const Document& doc = project.document;

  out << fmt::format("project     {} ({}, format {})\n", doc.name,
                     std::filesystem::is_directory(path) ? "folder" : "zip",
                     kProjectFormatVersion);
  out << fmt::format("hash        {}\n", content_hash(project).to_hex());
  out << fmt::format("layout      {} variables, {} features\n",
                     doc.layout.variables().variables().size(), doc.layout.features().size());
  out << fmt::format("blobs       {}\n", project.blobs.size());

  out << fmt::format("volumes ({})\n", doc.volumes.size());
  for (const auto& v : doc.volumes) {
    out << fmt::format("  {:<14} {:<11} boundary: {}\n", v.name, to_string(v.kind()),
                       boundary_text(doc, v));
    out << fmt::format("  {:<14} {:<11} contents: {}\n", "", "", contents_text(v));
    if (!v.parameters.empty()) {
      std::vector<std::string> ps;
      for (const auto& p : v.parameters) {
        ps.push_back(fmt::format("{} = {}", p.name, param_text(p.value)));
      }
      out << fmt::format("  {:<14} {:<11} parameters: {}\n", "", "", join(ps));
    }
    if (!v.outputs.empty()) {
      std::vector<std::string> os;
      for (const auto& o : v.outputs) os.push_back(fmt::format("{} ({})", o.name, to_string(o.quantity)));
      out << fmt::format("  {:<14} {:<11} outputs: {}\n", "", "", join(os));
    }
    if (v.seed != 0) out << fmt::format("  {:<14} {:<11} seed: {}\n", "", "", v.seed);
  }

  out << fmt::format("connection surfaces ({})\n", doc.connections.size());
  for (const auto& c : doc.connections) {
    std::vector<std::string> joined;
    for (const auto& id : c.volumes) joined.push_back(doc.volume_subject(id).label);
    if (c.volumes.size() == 1) joined.push_back("the outside");
    out << fmt::format("  {:<14} {:<13} joins {}; patch: {}\n", c.name, to_string(c.type()),
                       join(joined, " and "), geometry_text(doc, c));
    out << fmt::format("  {:<14} {}\n", "", params_text(c));
  }

  out << fmt::format("keep-outs ({})\n", doc.keep_outs.size());
  for (const auto& k : doc.keep_outs) {
    std::vector<std::string> cutters;
    for (const auto& id : k.cutters) {
      cutters.push_back(doc.find_volume(id) ? doc.volume_subject(id).label : doc.layout.label(id));
    }
    std::vector<std::string> cut;
    for (const auto& id : k.volumes) cut.push_back(doc.volume_subject(id).label);
    out << fmt::format("  {:<14} cuts {} with {}\n", k.name, join(cut), join(cutters));
  }

  out << fmt::format("links ({})\n", doc.links.size());
  for (const auto& l : doc.links) out << "  " << doc.link_subject(l.id).label << "\n";

  // Dependencies of everything but the layout's own features and variables,
  // whose order the feature tree already shows.
  const auto graph = DependencyGraph::build(doc);
  out << "dependencies (uses)\n";
  for (const std::size_t n : graph.order()) {
    const auto& node = graph.nodes()[n];
    if (node.kind == NodeKind::Feature || node.kind == NodeKind::Variable) continue;
    std::vector<std::string> inputs;
    for (const std::size_t i : graph.inputs(n)) inputs.push_back(graph.nodes()[i].description);
    out << fmt::format("  {} \xE2\x86\x90 {}\n", node.description,
                       inputs.empty() ? std::string("(nothing)") : join(inputs));
  }

  if (!do_build) return 0;
  const auto result = build(doc, core::ProductSettings::defaults(), &project.blobs);
  out << "build\n";
  for (std::size_t i = 0; i < doc.volumes.size(); ++i) {
    const auto& v = doc.volumes[i];
    const auto& vb = result.volumes[i];
    std::string boundary = "none";
    if (!vb.boundary.is_empty()) {
      if (const auto m = kernel::mass_properties(vb.boundary)) {
        boundary = fmt::format("{:.6g} mm³", m->volume);
      }
    }
    out << fmt::format("  {:<14} {:<12} boundary {}\n", v.name, to_string(vb.status), boundary);
    for (const auto& o : vb.outputs) {
      const auto* def = v.find_output(o.output);
      out << fmt::format("  {:<14} {:<12} {} = {}\n", "", "", def ? def->name : "?",
                         o.value ? value_text(*o.value) : std::string("(no value yet)"));
    }
  }
  for (const auto& c : result.connections) {
    out << fmt::format("  {:<14} {:<12} area {:.6g} mm², {} holes\n",
                       doc.connection_subject(c.surface).label, c.ok ? "ok" : "failed", c.area,
                       c.hole_centres.size());
  }
  for (const auto& d : result.diagnostics) out << "  " << d.to_string() << "\n";
  return result.ok() ? 0 : 1;
}

}  // namespace vs::cli

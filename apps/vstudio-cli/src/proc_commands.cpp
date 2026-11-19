#include "proc_commands.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <fmt/format.h>

#include "vs/proc/check.hpp"
#include "vs/proc/graph.hpp"
#include "vs/proc/nodes.hpp"

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

}  // namespace vs::cli

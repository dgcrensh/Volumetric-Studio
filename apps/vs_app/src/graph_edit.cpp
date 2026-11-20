#include "graph_edit.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <set>

#include "vs/model/hierarchy.hpp"
#include "vs/feature/expression.hpp"
#include "vs/proc/check.hpp"

namespace vs::app {

using proc::Graph;
using proc::NodeType;
using proc::PortType;
using proc::PropertyKind;
using proc::Setting;

Rgb port_color(PortType type) {
  switch (type) {
    case PortType::Scalar: return {0.55f, 0.58f, 0.62f};
    case PortType::Vector: return {0.62f, 0.42f, 0.85f};
    case PortType::Field: return {0.15f, 0.55f, 0.90f};
    case PortType::Mesh: return {0.20f, 0.68f, 0.38f};
    case PortType::Frames: return {0.92f, 0.58f, 0.12f};
    case PortType::Curves: return {0.88f, 0.30f, 0.55f};
    case PortType::List: return {0.60f, 0.52f, 0.35f};
    case PortType::Children: return {0.30f, 0.70f, 0.72f};
  }
  return {};
}

std::string node_title(std::string_view type) {
  if (type == "output") return "Output";
  const auto dot = type.find('.');
  std::string_view family = dot == std::string_view::npos ? std::string_view{} : type.substr(0, dot);
  std::string_view rest = dot == std::string_view::npos ? type : type.substr(dot + 1);
  std::string out;
  for (const char c : rest) out += c == '_' ? ' ' : c;
  // Names that read badly alone get their family.
  if (family == "convert") {
    if (rest == "field_to_mesh") out = "field to mesh";
    if (rest == "mesh_to_field") out = "mesh to field";
  } else if (family == "input" || family == "query" || family == "scatter" || family == "pattern" ||
             family == "output") {
    out = std::string(family) + " " + out;
  } else if (family == "list" || family == "frames" || family == "vector" || family == "logic") {
    out = std::string(family) + " " + out;
  } else if (family == "mesh" && (rest == "box" || rest == "cylinder" || rest == "union" ||
                                  rest == "difference" || rest == "intersection" || rest == "instances")) {
    out = "mesh " + out;
  }
  if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}

std::string unique_node_name(const Graph& graph, const std::string& title) {
  auto taken = [&](const std::string& n) {
    return std::any_of(graph.nodes.begin(), graph.nodes.end(), [&](const proc::Node& x) { return x.name == n; });
  };
  if (!taken(title)) return title;
  for (int i = 2;; ++i) {
    std::string n = fmt::format("{} {}", title, i);
    if (!taken(n)) return n;
  }
}

GraphContext graph_context(const model::Document& document, const Id& volume) {
  GraphContext c;
  for (const auto& s : document.connections) {
    if (model::joins(document, s, volume)) c.surfaces.push_back({s.id, s.name, s.type()});
  }
  // L4: a composite's templates, for its generator's children nodes.
  if (const model::Volume* v = document.find_volume(volume)) {
    if (const auto* comp = std::get_if<model::CompositeContents>(&v->contents)) {
      for (const auto& t : comp->templates) c.templates.push_back(t.name);
    }
  }
  return c;
}

bool GraphChange::ok() const {
  for (const auto& d : diagnostics) {
    if (d.is_error() && !proc::is_unfinished(d.code)) return false;
  }
  return true;
}

namespace {

std::string unique_parameter_name(const Graph& graph, const std::string& base) {
  std::string clean;
  for (const char c : base) clean += std::isalnum(static_cast<unsigned char>(c)) != 0 ? c : '_';
  if (clean.empty() || std::isdigit(static_cast<unsigned char>(clean[0])) != 0) clean = "p" + clean;
  if (graph.find_parameter(clean) == nullptr && feature::is_valid_name(clean)) return clean;
  for (int i = 2;; ++i) {
    std::string n = fmt::format("{}{}", clean, i);
    if (graph.find_parameter(n) == nullptr && feature::is_valid_name(n)) return n;
  }
}

// Applies `change` through proc::edit(); `label` names it.
GraphChange change_graph(const Graph& graph, std::string label, const std::function<void(Graph&)>& change) {
  GraphChange out;
  out.graph = graph;
  out.label = std::move(label);
  out.diagnostics = proc::edit(out.graph, change);
  return out;
}

GraphChange refused(const Graph& graph, std::string label, std::string code, std::string message) {
  GraphChange out;
  out.graph = graph;
  out.label = std::move(label);
  out.diagnostics.add(core::Diagnostic::error(std::move(code), std::move(message)));
  return out;
}

std::string node_label(const proc::Node& n) { return n.name.empty() ? n.type : n.name; }

}  // namespace

std::optional<std::map<std::string, Setting, std::less<>>> default_settings(const Graph& graph, const NodeType& type,
                                                                            const GraphContext& context,
                                                                            std::string* why) {
  std::map<std::string, Setting, std::less<>> settings;
  for (const auto& prop : type.properties) {
    if (prop.default_value) continue;
    switch (prop.kind) {
      case PropertyKind::Number: settings.emplace(prop.name, Setting(0.0)); break;
      case PropertyKind::Vector: settings.emplace(prop.name, Setting(core::dvec3{})); break;
      case PropertyKind::Choice:
        if (!prop.choices.empty()) settings.emplace(prop.name, Setting(prop.choices.front()));
        break;
      case PropertyKind::Surface:
        if (context.surfaces.empty()) {
          if (why != nullptr) *why = "No connection surface joins this volume, so there's nothing for it to read";
          return std::nullopt;
        }
        settings.emplace(prop.name, Setting(context.surfaces.front().id.to_string()));
        break;
      case PropertyKind::Parameter:
        // add_node() adds a parameter first when there is none.
        if (!graph.parameters.empty()) settings.emplace(prop.name, Setting(graph.parameters.front().name));
        break;
      case PropertyKind::Template:
      case PropertyKind::TemplateParameter: settings.emplace(prop.name, Setting(std::string())); break;
      case PropertyKind::Name: {
        std::set<std::string, std::less<>> used;
        for (const auto& n : graph.nodes) {
          if (const Setting* s = n.find_setting(prop.name); s != nullptr && std::holds_alternative<std::string>(*s)) {
            used.insert(std::get<std::string>(*s));
          }
        }
        std::string name = "value";
        for (int i = 2; used.contains(name); ++i) name = fmt::format("value{}", i);
        settings.emplace(prop.name, Setting(name));
        break;
      }
    }
  }
  return settings;
}

GraphChange add_node(const Graph& graph, std::string_view type_name, const GraphContext& context, const Id& id) {
  const std::string title = node_title(type_name);
  const std::string label = fmt::format("Add node {}", title);
  const NodeType* type = proc::library().find(type_name);
  if (type == nullptr) {
    return refused(graph, label, "proc.graph.unknown_type", fmt::format("There's no node type '{}'", type_name));
  }
  Graph base = graph;
  const bool needs_parameter = std::any_of(type->properties.begin(), type->properties.end(), [](const auto& p) {
    return p.kind == PropertyKind::Parameter && !p.default_value;
  });
  if (needs_parameter && base.parameters.empty()) base.parameters.push_back({"value", 0.0});
  std::string why;
  auto settings = default_settings(base, *type, context, &why);
  if (!settings) return refused(graph, label, "app.graph.no_surface", why);
  proc::Node node;
  node.id = id;
  node.type = std::string(type_name);
  node.name = unique_node_name(graph, title);
  node.settings = std::move(*settings);
  GraphChange out = change_graph(graph, label, [&](Graph& g) {
    g.parameters = base.parameters;
    g.nodes.push_back(node);
  });
  out.node = id;
  return out;
}

bool ports_compatible(const Graph& graph, const proc::PortRef& from, const proc::PortRef& to) {
  if (from.node == to.node) return false;
  const proc::Node* a = graph.find_node(from.node);
  const proc::Node* b = graph.find_node(to.node);
  if (a == nullptr || b == nullptr) return false;
  const NodeType* ta = proc::library().find(a->type);
  const NodeType* tb = proc::library().find(b->type);
  if (ta == nullptr || tb == nullptr) return false;
  const proc::PortSpec* out = ta->find_output(from.port);
  const proc::PortSpec* in = tb->find_input(to.port);
  return out != nullptr && in != nullptr && out->type == in->type;
}

GraphChange connect_ports(const Graph& graph, const proc::PortRef& a, const proc::PortRef& b) {
  // Which end is the output?
  // (A port name may be both an input and an output of its node, as a
  // shell's "field" is: the other end decides.)
  proc::PortRef from = a, to = b;
  auto type_of = [&](const proc::PortRef& p) -> const NodeType* {
    const proc::Node* n = graph.find_node(p.node);
    return n != nullptr ? proc::library().find(n->type) : nullptr;
  };
  const NodeType* ta = type_of(a);
  const NodeType* tb = type_of(b);
  if (ta != nullptr && tb != nullptr) {
    const bool forward = ta->find_output(a.port) != nullptr && tb->find_input(b.port) != nullptr;
    const bool backward = tb->find_output(b.port) != nullptr && ta->find_input(a.port) != nullptr;
    if (!forward && backward) std::swap(from, to);
  }
  const proc::Node* nf = graph.find_node(from.node);
  const proc::Node* nt = graph.find_node(to.node);
  const std::string label =
      nf != nullptr && nt != nullptr
          ? fmt::format("Connect {}.{} to {}.{}", node_label(*nf), from.port, node_label(*nt), to.port)
          : std::string("Connect");
  if (from.node == to.node) {
    return refused(graph, label, "proc.graph.loop", "A node's output can't feed its own input");
  }
  return change_graph(graph, label, [&](Graph& g) {
    std::erase_if(g.edges, [&](const proc::Edge& e) { return e.to == to; });
    // Its literal value no longer applies.
    if (proc::Node* n = g.find_node(to.node)) n->settings.erase(to.port);
    g.edges.push_back({from, to});
  });
}

GraphChange disconnect(const Graph& graph, const proc::Edge& edge) {
  const proc::Node* nt = graph.find_node(edge.to.node);
  const std::string label =
      nt != nullptr ? fmt::format("Disconnect {}.{}", node_label(*nt), edge.to.port) : std::string("Disconnect");
  return change_graph(graph, label, [&](Graph& g) { std::erase(g.edges, edge); });
}

GraphChange remove_nodes(const Graph& graph, const std::vector<Id>& nodes) {
  std::string label = "Delete nodes";
  if (nodes.size() == 1) {
    if (const proc::Node* n = graph.find_node(nodes.front())) label = fmt::format("Delete node {}", node_label(*n));
  }
  auto gone = [&](const Id& id) { return std::find(nodes.begin(), nodes.end(), id) != nodes.end(); };
  return change_graph(graph, label, [&](Graph& g) {
    std::erase_if(g.nodes, [&](const proc::Node& n) { return gone(n.id); });
    std::erase_if(g.edges, [&](const proc::Edge& e) { return gone(e.from.node) || gone(e.to.node); });
  });
}

GraphChange rename_node(const Graph& graph, const Id& node, std::string name) {
  const proc::Node* n = graph.find_node(node);
  if (n == nullptr) return refused(graph, "Rename node", "proc.graph.unknown_node", "There's no such node");
  const std::string label = fmt::format("Rename node {} to {}", node_label(*n), name);
  return change_graph(graph, label, [&](Graph& g) { g.find_node(node)->name = name; });
}

GraphChange set_setting(const Graph& graph, const Id& node, const std::string& key, std::optional<Setting> value) {
  const proc::Node* n = graph.find_node(node);
  if (n == nullptr) return refused(graph, "Set", "proc.graph.unknown_node", "There's no such node");
  const std::string label = fmt::format("Set {}.{}", node_label(*n), key);
  return change_graph(graph, label, [&](Graph& g) {
    proc::Node* m = g.find_node(node);
    if (value) {
      m->settings.insert_or_assign(key, *value);
    } else {
      m->settings.erase(key);
    }
  });
}

GraphChange add_parameter(const Graph& graph, std::string name, double value) {
  const std::string label = fmt::format("Add parameter {}", name);
  if (graph.find_parameter(name) != nullptr) {
    return refused(graph, label, "proc.graph.parameter", fmt::format("There's already a parameter '{}'", name));
  }
  return change_graph(graph, label, [&](Graph& g) { g.parameters.push_back({name, value}); });
}

GraphChange set_parameter(const Graph& graph, const std::string& name, std::string new_name, double value) {
  const std::string label =
      new_name == name ? fmt::format("Set parameter {}", name) : fmt::format("Rename parameter {} to {}", name, new_name);
  if (graph.find_parameter(name) == nullptr) {
    return refused(graph, label, "proc.graph.parameter", fmt::format("There's no parameter '{}'", name));
  }
  if (new_name != name && graph.find_parameter(new_name) != nullptr) {
    return refused(graph, label, "proc.graph.parameter", fmt::format("There's already a parameter '{}'", new_name));
  }
  return change_graph(graph, label, [&](Graph& g) {
    for (auto& p : g.parameters) {
      if (p.name == name) {
        p.name = new_name;
        p.value = value;
      }
    }
    if (new_name == name) return;
    for (auto& n : g.nodes) {
      const NodeType* t = proc::library().find(n.type);
      if (t == nullptr) continue;
      for (const auto& prop : t->properties) {
        if (prop.kind != PropertyKind::Parameter) continue;
        if (const Setting* s = n.find_setting(prop.name); s != nullptr && *s == Setting(name)) {
          n.settings.insert_or_assign(prop.name, Setting(new_name));
        }
      }
    }
  });
}

GraphChange remove_parameter(const Graph& graph, const std::string& name) {
  return change_graph(graph, fmt::format("Delete parameter {}", name),
                      [&](Graph& g) { std::erase_if(g.parameters, [&](const auto& p) { return p.name == name; }); });
}

std::optional<Setting> input_value(const proc::Node& node, const proc::PortSpec& port) {
  if (const Setting* s = node.find_setting(port.name)) return *s;
  return port.default_value;
}

GraphChange expose_input(const Graph& graph, const Id& node, const std::string& port) {
  const proc::Node* n = graph.find_node(node);
  const NodeType* t = n != nullptr ? proc::library().find(n->type) : nullptr;
  const proc::PortSpec* p = t != nullptr ? t->find_input(port) : nullptr;
  const std::string label = fmt::format("Expose {}.{}", n != nullptr ? node_label(*n) : std::string("?"), port);
  if (p == nullptr || p->type != PortType::Scalar) {
    return refused(graph, label, "app.graph.expose", "Only a number input can be exposed as a parameter");
  }
  if (graph.edge_into(node, port) != nullptr) {
    return refused(graph, label, "app.graph.expose", "The input is connected already");
  }
  double value = 0.0;
  if (const auto v = input_value(*n, *p); v && std::holds_alternative<double>(*v)) value = std::get<double>(*v);
  const std::string name = unique_parameter_name(graph, port);
  const Id reader = Id::random();
  GraphChange out = change_graph(graph, label, [&](Graph& g) {
    g.parameters.push_back({name, value});
    proc::Node r;
    r.id = reader;
    r.type = "input.parameter";
    r.name = unique_node_name(g, name);
    r.settings.emplace("name", Setting(name));
    g.nodes.push_back(std::move(r));
    g.find_node(node)->settings.erase(port);
    g.edges.push_back({{reader, "value"}, {node, port}});
  });
  out.node = reader;
  return out;
}

// --- marks -------------------------------------------------------------------------------------

namespace {

int rank(core::Severity s) {
  switch (s) {
    case core::Severity::Error: return 3;
    case core::Severity::Warning: return 2;
    case core::Severity::Info: return 1;
  }
  return 0;
}

}  // namespace

std::map<Id, NodeMark> node_marks(const Graph& graph, const core::Diagnostics& diagnostics) {
  std::map<Id, NodeMark> marks;
  auto mark = [&](const Id& id, core::Severity severity, const std::string& code, const std::string& message) -> NodeMark& {
    NodeMark& m = marks[id];
    if (m.code.empty() || rank(severity) > rank(m.severity)) {
      m.severity = severity;
      m.code = code;
    }
    if (!message.empty() && std::find(m.messages.begin(), m.messages.end(), message) == m.messages.end()) {
      m.messages.push_back(message);
    }
    return m;
  };
  for (const auto& d : diagnostics) {
    if (d.subjects.empty() || d.subjects.front().kind != core::SubjectKind::Node) continue;
    const proc::Node* n = graph.find_node(d.subjects.front().id);
    if (n == nullptr) continue;
    // proc.graph.missing_input is the unfinished state, marked below.
    if (d.code == "proc.graph.missing_input") continue;
    NodeMark& m = mark(n->id, d.severity, d.code, d.message);
    const NodeType* t = proc::library().find(n->type);
    if (t == nullptr) continue;
    for (std::size_t i = 1; i < d.subjects.size(); ++i) {
      for (const auto& p : t->inputs) {
        if (d.subjects[i].id == Id::derive(n->id, p.name) &&
            std::find(m.ports.begin(), m.ports.end(), p.name) == m.ports.end()) {
          m.ports.push_back(p.name);
        }
      }
    }
  }
  // Inputs that must be connected and aren't: unfinished.
  for (const auto& n : graph.nodes) {
    const NodeType* t = proc::library().find(n.type);
    if (t == nullptr) continue;
    for (const auto& p : t->inputs) {
      if (p.optional || graph.edge_into(n.id, p.name) != nullptr) continue;
      const bool literal = (p.type == PortType::Scalar || p.type == PortType::Vector) &&
                           (p.default_value || n.find_setting(p.name) != nullptr);
      if (literal) continue;
      NodeMark& m = mark(n.id, core::Severity::Warning, "proc.graph.missing_input",
                         fmt::format("Input '{}' ({}) isn't connected yet", p.name, proc::to_string(p.type)));
      if (std::find(m.ports.begin(), m.ports.end(), p.name) == m.ports.end()) m.ports.push_back(p.name);
    }
  }
  return marks;
}

core::Diagnostics graph_diagnostics(const Graph& graph, const model::Document& document, const Id& volume,
                                    const core::Diagnostics& evaluation) {
  core::Diagnostics out = proc::check_volume_graph(graph, document, volume);
  for (const auto& d : evaluation) {
    if (d.subjects.empty() || d.subjects.front().kind != core::SubjectKind::Node) continue;
    if (graph.find_node(d.subjects.front().id) == nullptr) continue;
    bool seen = false;
    for (const auto& x : out) seen = seen || (x.code == d.code && x.message == d.message);
    if (!seen) out.add(d);
  }
  return out;
}

// --- layout ----------------------------------------------------------------------------------

std::map<Id, NodePoint> layered_layout(const Graph& graph, double column, double row) {
  // Depth: the longest path from a node with no inputs connected.
  std::map<Id, int> depth;
  for (const auto& n : graph.nodes) depth[n.id] = 0;
  const auto order = proc::topological_order(graph);
  if (!order.empty()) {
    for (const std::size_t i : order) {
      const Id& id = graph.nodes[i].id;
      for (const auto& e : graph.edges) {
        if (e.from.node == id && depth.contains(e.to.node)) {
          depth[e.to.node] = std::max(depth[e.to.node], depth[id] + 1);
        }
      }
    }
  }
  // Rows stack by each node's height (as the canvas draws it: a header,
  // a row per port, a line of properties) and `row` between them.
  std::map<int, double> next_y;
  std::map<Id, NodePoint> out;
  for (const auto& n : graph.nodes) {
    const int d = depth[n.id];
    double& y = next_y[d];
    out[n.id] = {d * column, y};
    std::size_t ports = 1;
    if (const proc::NodeType* t = proc::library().find(n.type)) ports = std::max(t->inputs.size(), t->outputs.size());
    y += 26.0 + 20.0 * static_cast<double>(ports) + 22.0 + row;
  }
  return out;
}

}  // namespace vs::app

#include "vs/proc/check.hpp"
#include "vs/model/hierarchy.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include <fmt/format.h>

#include "vs/feature/expression.hpp"

namespace vs::proc {

using core::Diagnostic;
using core::Diagnostics;
using core::SubjectRef;

namespace {

std::string_view setting_kind(const Setting& s) {
  if (std::holds_alternative<double>(s)) return "a number";
  if (std::holds_alternative<core::dvec3>(s)) return "a vector";
  return "text";
}

// Checks one node's settings against its type.
void check_settings(const Graph& graph, const Node& node, const NodeType& type, Diagnostics& out) {
  const SubjectRef self = node_subject(node);
  for (const auto& [key, value] : node.settings) {
    if (const PortSpec* port = type.find_input(key)) {
      const bool ok = (port->type == PortType::Scalar && std::holds_alternative<double>(value)) ||
                      (port->type == PortType::Vector && std::holds_alternative<core::dvec3>(value));
      if (!ok) {
        out.add(Diagnostic::error(
            "proc.graph.setting",
            fmt::format("'{}': input '{}' is a {} and can't be set to {}", self.label, key,
                        to_string(port->type), setting_kind(value)),
            {self}));
      }
      continue;
    }
    const PropertySpec* prop = type.find_property(key);
    if (prop == nullptr) {
      out.add(Diagnostic::error(
          "proc.graph.setting",
          fmt::format("'{}': a {} has no input or property '{}'", self.label, type.type, key),
          {self}));
      continue;
    }
    bool ok = false;
    switch (prop->kind) {
      case PropertyKind::Number:
        ok = std::holds_alternative<double>(value);
        break;
      case PropertyKind::Vector:
        ok = std::holds_alternative<core::dvec3>(value);
        break;
      case PropertyKind::Choice: {
        const auto* s = std::get_if<std::string>(&value);
        ok = s != nullptr &&
             std::find(prop->choices.begin(), prop->choices.end(), *s) != prop->choices.end();
        break;
      }
      case PropertyKind::Surface: {
        const auto* s = std::get_if<std::string>(&value);
        ok = s != nullptr && Id::parse(*s).has_value();
        break;
      }
      case PropertyKind::Name: {
        const auto* s = std::get_if<std::string>(&value);
        ok = s != nullptr && feature::is_valid_name(*s);
        break;
      }
      case PropertyKind::Parameter: {
        const auto* s = std::get_if<std::string>(&value);
        ok = s != nullptr;
        if (ok && graph.find_parameter(*s) == nullptr) {
          out.add(Diagnostic::error(
              "proc.graph.parameter",
              fmt::format("'{}' reads parameter '{}', which the graph doesn't have", self.label, *s),
              {self}));
        }
        break;
      }
    }
    if (!ok) {
      out.add(Diagnostic::error(
          "proc.graph.setting",
          fmt::format("'{}': property '{}' can't be {}", self.label, key, setting_kind(value)),
          {self}));
    }
  }
  for (const auto& prop : type.properties) {
    if (!prop.default_value && node.find_setting(prop.name) == nullptr) {
      out.add(Diagnostic::error(
          "proc.graph.setting",
          fmt::format("'{}': property '{}' must be set", self.label, prop.name), {self}));
    }
  }
}

// The nodes on one loop, in edge order, from a graph with a loop.
std::vector<std::size_t> find_loop(const Graph& graph) {
  const std::size_t n = graph.nodes.size();
  std::vector<std::vector<std::size_t>> next(n);
  const auto index_of = [&](const Id& id) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < n; ++i) {
      if (graph.nodes[i].id == id) return i;
    }
    return std::nullopt;
  };
  for (const auto& e : graph.edges) {
    const auto a = index_of(e.from.node);
    const auto b = index_of(e.to.node);
    if (a && b) next[*a].push_back(*b);
  }
  std::vector<int> state(n, 0);  // 0 new, 1 on the stack, 2 done
  std::vector<std::size_t> stack;
  std::vector<std::size_t> loop;
  const std::function<bool(std::size_t)> visit = [&](std::size_t v) {
    state[v] = 1;
    stack.push_back(v);
    for (const std::size_t w : next[v]) {
      if (state[w] == 1) {
        const auto it = std::find(stack.begin(), stack.end(), w);
        loop.assign(it, stack.end());
        return true;
      }
      if (state[w] == 0 && visit(w)) return true;
    }
    stack.pop_back();
    state[v] = 2;
    return false;
  };
  for (std::size_t v = 0; v < n; ++v) {
    if (state[v] == 0 && visit(v)) break;
  }
  return loop;
}

}  // namespace

bool is_unfinished(std::string_view code) {
  return code == "proc.graph.missing_input" || code == "proc.graph.output";
}

std::vector<std::size_t> topological_order(const Graph& graph) {
  const std::size_t n = graph.nodes.size();
  std::vector<std::size_t> indegree(n, 0);
  std::vector<std::vector<std::size_t>> next(n);
  std::map<Id, std::size_t> index;
  for (std::size_t i = 0; i < n; ++i) index.emplace(graph.nodes[i].id, i);
  for (const auto& e : graph.edges) {
    const auto a = index.find(e.from.node);
    const auto b = index.find(e.to.node);
    if (a == index.end() || b == index.end()) continue;
    next[a->second].push_back(b->second);
    ++indegree[b->second];
  }
  // Kahn's algorithm, always taking the earliest ready node.
  std::set<std::size_t> ready;
  for (std::size_t i = 0; i < n; ++i) {
    if (indegree[i] == 0) ready.insert(i);
  }
  std::vector<std::size_t> order;
  while (!ready.empty()) {
    const std::size_t v = *ready.begin();
    ready.erase(ready.begin());
    order.push_back(v);
    for (const std::size_t w : next[v]) {
      if (--indegree[w] == 0) ready.insert(w);
    }
  }
  if (order.size() != n) return {};
  return order;
}

Diagnostics check_graph(const Graph& graph, const NodeRegistry& registry) {
  Diagnostics out;

  // Parameters.
  std::set<std::string, std::less<>> parameter_names;
  for (const auto& p : graph.parameters) {
    if (!feature::is_valid_name(p.name)) {
      out.add(Diagnostic::error("proc.graph.parameter",
                                fmt::format("'{}' isn't a legal parameter name", p.name)));
    } else if (!parameter_names.insert(p.name).second) {
      out.add(Diagnostic::error("proc.graph.parameter",
                                fmt::format("Parameter '{}' is declared twice", p.name)));
    } else if (!std::isfinite(p.value)) {
      out.add(Diagnostic::error("proc.graph.parameter",
                                fmt::format("Parameter '{}' has no finite value", p.name)));
    }
  }

  // Nodes.
  std::set<Id> ids;
  std::size_t outputs = 0;
  for (const auto& node : graph.nodes) {
    const SubjectRef self = node_subject(node);
    if (!ids.insert(node.id).second || node.id.is_null()) {
      out.add(Diagnostic::error("proc.graph.duplicate_id",
                                fmt::format("'{}': its id {} isn't unique", self.label,
                                            node.id.to_string()),
                                {self}));
    }
    const NodeType* type = registry.find(node.type);
    if (type == nullptr) {
      out.add(Diagnostic::error("proc.graph.unknown_type",
                                fmt::format("'{}': there's no node type '{}'", self.label, node.type),
                                {self}));
      continue;
    }
    if (node.type == "output") ++outputs;
    check_settings(graph, node, *type, out);
  }

  // Published names: each once.
  {
    std::map<std::string, const Node*, std::less<>> published;
    for (const auto& node : graph.nodes) {
      if (node.type != "output.publish") continue;
      const Setting* s = node.find_setting("name");
      const auto* name = s != nullptr ? std::get_if<std::string>(s) : nullptr;
      if (name == nullptr) continue;  // reported as a setting
      const auto [it, fresh] = published.emplace(*name, &node);
      if (!fresh) {
        out.add(Diagnostic::error(
            "proc.graph.publish",
            fmt::format("'{}' publishes '{}', which '{}' already publishes",
                        node_subject(node).label, *name, node_subject(*it->second).label),
            {node_subject(node), node_subject(*it->second)}));
      }
    }
  }

  // Edges.
  std::set<std::pair<Id, std::string>> taken;
  for (const auto& e : graph.edges) {
    const Node* from = graph.find_node(e.from.node);
    const Node* to = graph.find_node(e.to.node);
    if (from == nullptr || to == nullptr) {
      const Node* known = from != nullptr ? from : to;
      std::vector<SubjectRef> subjects;
      if (known != nullptr) subjects.push_back(node_subject(*known));
      out.add(Diagnostic::error(
          "proc.graph.unknown_node",
          fmt::format("An edge names node {}, which isn't in the graph",
                      (from == nullptr ? e.from.node : e.to.node).to_string()),
          std::move(subjects)));
      continue;
    }
    const NodeType* from_type = registry.find(from->type);
    const NodeType* to_type = registry.find(to->type);
    if (from_type == nullptr || to_type == nullptr) continue;  // reported above
    const PortSpec* out_port = from_type->find_output(e.from.port);
    const PortSpec* in_port = to_type->find_input(e.to.port);
    if (out_port == nullptr) {
      out.add(Diagnostic::error(
          "proc.graph.unknown_port",
          fmt::format("'{}' has no output '{}'", node_subject(*from).label, e.from.port),
          {node_subject(*from)}));
      continue;
    }
    if (in_port == nullptr) {
      out.add(Diagnostic::error(
          "proc.graph.unknown_port",
          fmt::format("'{}' has no input '{}'", node_subject(*to).label, e.to.port),
          {node_subject(*to)}));
      continue;
    }
    if (out_port->type != in_port->type) {
      out.add(Diagnostic::error(
          "proc.graph.type_mismatch",
          fmt::format("'{}.{}' takes a {}, but '{}.{}' gives a {}; convert it with a node",
                      node_subject(*to).label, e.to.port, to_string(in_port->type),
                      node_subject(*from).label, e.from.port, to_string(out_port->type)),
          {node_subject(*to), node_subject(*from)}));
    }
    if (!taken.insert({e.to.node, e.to.port}).second) {
      out.add(Diagnostic::error(
          "proc.graph.input_taken",
          fmt::format("'{}.{}' already has an edge into it", node_subject(*to).label, e.to.port),
          {node_subject(*to)}));
    }
  }

  // Inputs that need an edge.
  for (const auto& node : graph.nodes) {
    const NodeType* type = registry.find(node.type);
    if (type == nullptr) continue;
    for (const auto& port : type->inputs) {
      if (taken.contains({node.id, port.name}) || port.optional) continue;
      const bool literal = (port.type == PortType::Scalar || port.type == PortType::Vector) &&
                           (port.default_value || node.find_setting(port.name) != nullptr);
      if (!literal) {
        out.add(Diagnostic::error(
            "proc.graph.missing_input",
            fmt::format("'{}': input '{}' ({}) must be connected", node_subject(node).label,
                        port.name, to_string(port.type)),
            {node_subject(node)}));
      }
    }
  }

  // Loops.
  if (topological_order(graph).size() != graph.nodes.size()) {
    const auto loop = find_loop(graph);
    std::vector<SubjectRef> subjects;
    std::string path;
    for (const std::size_t i : loop) {
      subjects.push_back(node_subject(graph.nodes[i]));
      path += subjects.back().label + " -> ";
    }
    if (!loop.empty()) path += node_subject(graph.nodes[loop.front()]).label;
    out.add(Diagnostic::error("proc.graph.loop",
                              fmt::format("The graph has a loop: {}", path), std::move(subjects)));
  }

  if (outputs != 1) {
    out.add(Diagnostic::error(
        "proc.graph.output",
        outputs == 0 ? std::string("The graph has no output node")
                     : fmt::format("The graph has {} output nodes; it needs exactly one", outputs)));
  }
  return out;
}

Diagnostics check_volume_graph(const Graph& graph, const model::Document& document,
                               const Id& volume, const NodeRegistry& registry) {
  Diagnostics out = check_graph(graph, registry);
  for (const auto& node : graph.nodes) {
    const NodeType* type = registry.find(node.type);
    if (type == nullptr) continue;
    for (const auto& prop : type->properties) {
      if (prop.kind != PropertyKind::Surface) continue;
      const Setting* s = node.find_setting(prop.name);
      const auto* text = s != nullptr ? std::get_if<std::string>(s) : nullptr;
      const auto id = text != nullptr ? Id::parse(*text) : std::nullopt;
      if (!id) continue;  // check_graph() reported it
      const model::ConnectionSurface* surface = document.find_connection(*id);
      if (surface == nullptr || !model::joins(document, *surface, volume)) {
        out.add(Diagnostic::error(
            "proc.reference.surface",
            fmt::format("'{}' reads connection surface {}, which doesn't join this volume; a graph "
                        "sees only its own volume's inputs",
                        node_subject(node).label,
                        surface != nullptr ? "'" + surface->name + "'" : id->to_string()),
            {node_subject(node)}));
      }
    }
  }
  return out;
}

Diagnostics edit(Graph& graph, const std::function<void(Graph&)>& change,
                 const NodeRegistry& registry) {
  Graph copy = graph;
  change(copy);
  Diagnostics d = check_graph(copy, registry);
  const bool wrong = std::any_of(d.begin(), d.end(), [](const Diagnostic& x) {
    return x.is_error() && !is_unfinished(x.code);
  });
  if (!wrong) graph = std::move(copy);
  return d;
}

Diagnostics connect(Graph& graph, const PortRef& from, const PortRef& to,
                    const NodeRegistry& registry) {
  return edit(
      graph, [&](Graph& g) { g.edges.push_back({from, to}); }, registry);
}

}  // namespace vs::proc

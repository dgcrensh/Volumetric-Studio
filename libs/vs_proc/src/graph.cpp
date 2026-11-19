#include "vs/proc/graph.hpp"

#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace vs::proc {

using core::Diagnostic;
using core::SubjectKind;
using core::SubjectRef;
using ojson = nlohmann::ordered_json;

const Setting* Node::find_setting(std::string_view key) const {
  const auto it = settings.find(key);
  return it == settings.end() ? nullptr : &it->second;
}

const Node* Graph::find_node(const Id& id) const {
  for (const auto& n : nodes) {
    if (n.id == id) return &n;
  }
  return nullptr;
}

Node* Graph::find_node(const Id& id) {
  for (auto& n : nodes) {
    if (n.id == id) return &n;
  }
  return nullptr;
}

const Node* Graph::find_node(std::string_view name) const {
  for (const auto& n : nodes) {
    if (n.name == name) return &n;
  }
  return nullptr;
}

const GraphParameter* Graph::find_parameter(std::string_view name) const {
  for (const auto& p : parameters) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

const Edge* Graph::edge_into(const Id& node, std::string_view port) const {
  for (const auto& e : edges) {
    if (e.to.node == node && e.to.port == port) return &e;
  }
  return nullptr;
}

Id Graph::add(std::string type, std::string name,
              std::map<std::string, Setting, std::less<>> settings) {
  Node n;
  n.id = Id::random();
  n.type = std::move(type);
  n.name = std::move(name);
  n.settings = std::move(settings);
  nodes.push_back(std::move(n));
  return nodes.back().id;
}

void Graph::link(const Id& from, std::string from_port, const Id& to, std::string to_port) {
  edges.push_back({{from, std::move(from_port)}, {to, std::move(to_port)}});
}

SubjectRef node_subject(const Node& node) {
  return {SubjectKind::Node, node.id, node.name.empty() ? node.type : node.name};
}

// --- JSON ----------------------------------------------------------------------------

namespace {

ojson setting_json(const Setting& s) {
  if (const auto* d = std::get_if<double>(&s)) return *d;
  if (const auto* v = std::get_if<core::dvec3>(&s)) return ojson::array({v->x, v->y, v->z});
  return std::get<std::string>(s);
}

ojson port_json(const PortRef& p) { return ojson{{"node", p.node.to_string()}, {"port", p.port}}; }

struct ParseError {
  std::string message;
};

Id parse_id(const nlohmann::json& j, std::string_view where) {
  if (!j.is_string()) throw ParseError{fmt::format("{}: an id must be a string", where)};
  const auto id = Id::parse(j.get<std::string>());
  if (!id) throw ParseError{fmt::format("{}: '{}' isn't an id", where, j.get<std::string>())};
  return *id;
}

const nlohmann::json& member(const nlohmann::json& j, const char* key, std::string_view where) {
  if (!j.is_object() || !j.contains(key)) {
    throw ParseError{fmt::format("{}: '{}' is missing", where, key)};
  }
  return j.at(key);
}

std::string string_member(const nlohmann::json& j, const char* key, std::string_view where) {
  const auto& v = member(j, key, where);
  if (!v.is_string()) throw ParseError{fmt::format("{}: '{}' must be a string", where, key)};
  return v.get<std::string>();
}

double number(const nlohmann::json& j, std::string_view where) {
  if (!j.is_number()) throw ParseError{fmt::format("{}: expected a number", where)};
  return j.get<double>();
}

Setting parse_setting(const nlohmann::json& j, std::string_view where) {
  if (j.is_number()) return j.get<double>();
  if (j.is_string()) return j.get<std::string>();
  if (j.is_array() && j.size() == 3) {
    return core::dvec3{number(j[0], where), number(j[1], where), number(j[2], where)};
  }
  throw ParseError{fmt::format("{}: a setting is a number, three numbers or a string", where)};
}

PortRef parse_port(const nlohmann::json& j, std::string_view where) {
  return {parse_id(member(j, "node", where), where), string_member(j, "port", where)};
}

}  // namespace

std::string to_json(const Graph& graph) {
  ojson j;
  j["format"] = "vstudio.graph";
  j["version"] = kGraphFormatVersion;
  ojson params = ojson::array();
  for (const auto& p : graph.parameters) params.push_back(ojson{{"name", p.name}, {"value", p.value}});
  j["parameters"] = std::move(params);
  ojson nodes = ojson::array();
  for (const auto& n : graph.nodes) {
    ojson settings = ojson::object();
    for (const auto& [k, v] : n.settings) settings[k] = setting_json(v);
    nodes.push_back(ojson{{"id", n.id.to_string()},
                          {"type", n.type},
                          {"name", n.name},
                          {"settings", std::move(settings)}});
  }
  j["nodes"] = std::move(nodes);
  ojson edges = ojson::array();
  for (const auto& e : graph.edges) {
    edges.push_back(ojson{{"from", port_json(e.from)}, {"to", port_json(e.to)}});
  }
  j["edges"] = std::move(edges);
  return j.dump(2);
}

core::Result<Graph> graph_from_json(std::string_view text) {
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (j.is_discarded()) {
    return Diagnostic::error("proc.json.parse", "The graph isn't valid JSON");
  }
  try {
    if (string_member(j, "format", "graph") != "vstudio.graph") {
      return Diagnostic::error("proc.json.version", "This isn't a vstudio graph");
    }
    const auto& version = member(j, "version", "graph");
    if (!version.is_number_integer() || version.get<int>() != kGraphFormatVersion) {
      return Diagnostic::error(
          "proc.json.version",
          fmt::format("Graph format version {} can't be read (this build reads {})", version.dump(),
                      kGraphFormatVersion));
    }
    Graph g;
    if (j.contains("parameters")) {
      for (const auto& p : j.at("parameters")) {
        g.parameters.push_back(
            {string_member(p, "name", "parameter"), number(member(p, "value", "parameter"), "parameter")});
      }
    }
    for (const auto& jn : member(j, "nodes", "graph")) {
      Node n;
      n.id = parse_id(member(jn, "id", "node"), "node");
      const std::string where = fmt::format("node {}", n.id.to_string());
      n.type = string_member(jn, "type", where);
      if (jn.contains("name")) n.name = string_member(jn, "name", where);
      if (jn.contains("settings")) {
        const auto& s = jn.at("settings");
        if (!s.is_object()) throw ParseError{fmt::format("{}: settings must be an object", where)};
        for (const auto& [k, v] : s.items()) {
          n.settings.emplace(k, parse_setting(v, fmt::format("{} setting '{}'", where, k)));
        }
      }
      g.nodes.push_back(std::move(n));
    }
    if (j.contains("edges")) {
      for (const auto& je : j.at("edges")) {
        g.edges.push_back({parse_port(member(je, "from", "edge"), "edge from"),
                           parse_port(member(je, "to", "edge"), "edge to")});
      }
    }
    return g;
  } catch (const ParseError& e) {
    return Diagnostic::error("proc.json.parse", e.message);
  } catch (const nlohmann::json::exception& e) {
    return Diagnostic::error("proc.json.parse", e.what());
  }
}

}  // namespace vs::proc

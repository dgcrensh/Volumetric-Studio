#pragma once

// The procedural graph model (spec, "Procedural system"; plan, M10).
//
// A Graph is what a procedural volume stores (model::ProceduralContents
// holds its JSON). It has:
//
//   - parameters: the numbers it exposes, each with a default. A volume
//     parameter of the same name overrides the default, so layout
//     variables and links can drive the graph through the volume;
//   - nodes, each with a stable id, a type from the node library
//     (nodes.hpp), a name for messages, and settings: literal values for
//     its unconnected Scalar and Vector inputs and for its properties
//     (configuration that isn't a port, such as which connection surface
//     it reads);
//   - edges, each from one node's output port to another node's input
//     port. An input takes at most one edge; an output feeds any number.
//
// Exactly one node is of type "output": what reaches its input is the
// volume's contents.
//
// Graphs are authored through edit() and connect() (check.hpp), which
// keep a change only if check_graph() finds no errors: type mismatches and
// loops are rejected when they are made, not when the graph runs.
//
// JSON (to_json() / graph_from_json()) round-trips exactly, ids included:
//
//   {"format": "vstudio.graph", "version": 1,
//    "parameters": [{"name": "wall", "value": 2}],
//    "nodes": [{"id": "<32 hex>", "type": "field.sphere", "name": "Ball",
//               "settings": {"radius": 5, "centre": [0, 0, 0], "surface": "<32 hex>"}}],
//    "edges": [{"from": {"node": "<id>", "port": "field"},
//               "to": {"node": "<id>", "port": "a"}}]}
//
// A setting is a number, a three-number array (a vector) or a string.
//   proc.json.parse      not JSON, or not a graph (the message says where)
//   proc.json.version    a format or version this build doesn't read

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/core/vec.hpp"

namespace vs::proc {

using core::Id;

using Setting = std::variant<double, core::dvec3, std::string>;

struct PortRef {
  Id node;
  std::string port;
  friend bool operator==(const PortRef&, const PortRef&) = default;
};

struct Edge {
  PortRef from;  // an output
  PortRef to;    // an input
  friend bool operator==(const Edge&, const Edge&) = default;
};

struct Node {
  Id id;
  std::string type;
  std::string name;
  std::map<std::string, Setting, std::less<>> settings;

  [[nodiscard]] const Setting* find_setting(std::string_view key) const;
  friend bool operator==(const Node&, const Node&) = default;
};

struct GraphParameter {
  std::string name;  // a legal variable name
  double value = 0.0;
  friend bool operator==(const GraphParameter&, const GraphParameter&) = default;
};

struct Graph {
  std::vector<GraphParameter> parameters;
  std::vector<Node> nodes;
  std::vector<Edge> edges;

  [[nodiscard]] const Node* find_node(const Id& id) const;
  [[nodiscard]] Node* find_node(const Id& id);
  [[nodiscard]] const Node* find_node(std::string_view name) const;
  [[nodiscard]] const GraphParameter* find_parameter(std::string_view name) const;
  // The edge into `node`'s input `port`, if any (the first, if a broken
  // graph has several).
  [[nodiscard]] const Edge* edge_into(const Id& node, std::string_view port) const;

  // Adds a node with a fresh random id and returns the id.
  Id add(std::string type, std::string name = {},
         std::map<std::string, Setting, std::less<>> settings = {});
  // Adds an edge without checking it (see connect() in check.hpp).
  void link(const Id& from, std::string from_port, const Id& to, std::string to_port);

  friend bool operator==(const Graph&, const Graph&) = default;
};

// A subject naming a node in diagnostics.
core::SubjectRef node_subject(const Node& node);

inline constexpr int kGraphFormatVersion = 1;

std::string to_json(const Graph& graph);
core::Result<Graph> graph_from_json(std::string_view text);

}  // namespace vs::proc

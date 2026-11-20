#pragma once

// The node editor's rules (M15), as plain data: no Qt. The canvas
// (node_editor.hpp) only draws a graph and turns gestures into these calls;
// each returns the changed graph, which the main window applies as one
// undoable Command (authoring.hpp: set_graph()).
//
// Every change goes through proc::edit(), so a graph never holds a type
// error or a loop, yet can be built one node at a time (an input not
// connected yet is "unfinished", not wrong). A refused change returns the
// graph unchanged with the reason (proc.graph.*).
//
// Also here: the colour of each port type, the default name and settings
// of a new node, the marks the canvas puts on nodes from diagnostics, and
// the layered layout used for nodes the user hasn't placed.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scene.hpp"
#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/model/document.hpp"
#include "vs/proc/graph.hpp"
#include "vs/proc/nodes.hpp"
#include "vs/proc/types.hpp"

namespace vs::app {

using core::Id;

// The colour ports and edges of `type` are drawn with.
Rgb port_color(proc::PortType type);

// "Shell" for field.shell, "Field to mesh" for convert.field_to_mesh, "Output".
std::string node_title(std::string_view type);

// `title`, or `title 2`, `title 3`, ... whichever no node of `graph` is named.
std::string unique_node_name(const proc::Graph& graph, const std::string& title);

// What a volume's graph may read: the connection surfaces joining it.
struct GraphContext {
  struct Surface {
    Id id;
    std::string name;
    model::ConnectionType type = model::ConnectionType::Bonded;
  };
  std::vector<Surface> surfaces;
  std::vector<std::string> templates;  // L4: a composite's, by name
};
GraphContext graph_context(const model::Document& document, const Id& volume);

// A change's result: the new graph (the old one when refused) and why.
struct GraphChange {
  proc::Graph graph;
  core::Diagnostics diagnostics;
  std::string label;  // for the undo menu: "Add node Shell", ...
  Id node;            // the node added (add_node, expose_input)

  // Refused: the graph has errors other than unfinished ones.
  [[nodiscard]] bool ok() const;
};

// The settings a new node of `type` starts with: every property that has
// no default gets one it can work with (the first graph parameter, the
// first joining surface, the first choice, a fresh published name).
// nullopt when there is none to give (a surface node with no surface
// joining the volume), with the reason in `why`.
std::optional<std::map<std::string, proc::Setting, std::less<>>> default_settings(
    const proc::Graph& graph, const proc::NodeType& type, const GraphContext& context, std::string* why = nullptr);

// Adds a node of `type` (named after it) with default settings. A
// parameter node in a graph with no parameters first adds one.
GraphChange add_node(const proc::Graph& graph, std::string_view type, const GraphContext& context,
                     const Id& id = Id::random());

// Joins an output to an input; an edge already into the input is replaced.
// The ports may be given either way round.
GraphChange connect_ports(const proc::Graph& graph, const proc::PortRef& a, const proc::PortRef& b);
GraphChange disconnect(const proc::Graph& graph, const proc::Edge& edge);
// Removes nodes and every edge touching them.
GraphChange remove_nodes(const proc::Graph& graph, const std::vector<Id>& nodes);
GraphChange rename_node(const proc::Graph& graph, const Id& node, std::string name);
// Sets a node's setting (an unconnected input's value or a property);
// nullopt removes it (back to the port's default).
GraphChange set_setting(const proc::Graph& graph, const Id& node, const std::string& key,
                        std::optional<proc::Setting> value);

// Exposed parameters: the numbers a volume's parameters of the same name
// override.
GraphChange add_parameter(const proc::Graph& graph, std::string name, double value);
// Renames and/or revalues `name`; parameter nodes reading it follow a rename.
GraphChange set_parameter(const proc::Graph& graph, const std::string& name, std::string new_name, double value);
GraphChange remove_parameter(const proc::Graph& graph, const std::string& name);
// Exposes an unconnected Scalar input: a new parameter (named after the
// port, made unique) with the input's current value, a parameter node
// reading it, and an edge into the input.
GraphChange expose_input(const proc::Graph& graph, const Id& node, const std::string& port);

// Whether an edge from output `from` to input `to` could be made: both
// ports exist, their types match and they are on different nodes. (Loops
// are caught when the edge is made.)
bool ports_compatible(const proc::Graph& graph, const proc::PortRef& from, const proc::PortRef& to);

// The value an unconnected Scalar or Vector input has: its setting, else
// the port's default.
std::optional<proc::Setting> input_value(const proc::Node& node, const proc::PortSpec& port);

// --- marks on the canvas ---------------------------------------------------------------------

struct NodeMark {
  core::Severity severity = core::Severity::Info;
  std::string code;                   // the worst diagnostic's
  std::vector<std::string> messages;  // every diagnostic naming the node first
  std::vector<std::string> ports;     // inputs named (infeasible), or not connected yet
  [[nodiscard]] bool failed() const { return severity == core::Severity::Error; }
  friend bool operator==(const NodeMark&, const NodeMark&) = default;
};

// Marks from diagnostics whose first subject is a node of `graph`
// (proc.node.infeasible names its inputs after it), and a warning on every
// node with an input that must be connected and isn't.
std::map<Id, NodeMark> node_marks(const proc::Graph& graph, const core::Diagnostics& diagnostics);

// The diagnostics of a volume's graph: check_volume_graph() and what the
// last evaluation said about the volume.
core::Diagnostics graph_diagnostics(const proc::Graph& graph, const model::Document& document, const Id& volume,
                                    const core::Diagnostics& evaluation);

// --- layout -------------------------------------------------------------------------------------

struct NodePoint {
  double x = 0, y = 0;
  friend bool operator==(const NodePoint&, const NodePoint&) = default;
};

// Columns by depth (inputs on the left, the output on the right), rows in
// graph order, stacked by each node's drawn height with `row` between:
// for nodes the user hasn't placed.
std::map<Id, NodePoint> layered_layout(const proc::Graph& graph, double column = 250.0, double row = 30.0);

}  // namespace vs::app

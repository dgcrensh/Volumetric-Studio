#pragma once

// Authoring-time checks of procedural graphs (plan, M10: "type checking at
// authoring time"; acceptance: "type errors and graph loops are rejected at
// authoring time").
//
// check_graph() finds every structural problem; edit() and connect() apply
// a change to a copy and keep it only when check_graph() finds no errors
// other than the two that only say the graph is unfinished (an input not
// connected yet, no output node yet: is_unfinished()), so an authored graph
// never holds a type error or a loop, yet can be built one node at a time. The evaluator
// checks again before running (a graph may arrive from a file), and the
// procedural generator checks a volume's graph against its document.
//
// Codes (the node to act on first):
//   proc.graph.duplicate_id     two nodes share an id
//   proc.graph.unknown_type     a node's type isn't in the library
//   proc.graph.unknown_node     an edge names a node that isn't there
//   proc.graph.unknown_port     an edge names a port its node doesn't have
//                               (or an output as an input, or the reverse)
//   proc.graph.type_mismatch    an edge joins ports of different types (no
//                               implicit conversions: use a conversion node)
//   proc.graph.input_taken      a second edge into one input
//   proc.graph.missing_input    an input that must be connected isn't (or a
//                               Scalar/Vector input with no setting or default)
//   proc.graph.setting          a setting that isn't an input or property of
//                               the node, or has the wrong kind, or isn't one
//                               of a choice's options, or a required property
//                               is missing
//   proc.graph.parameter        a parameter node names no graph parameter, or a
//                               graph parameter's name is illegal or repeated
//                               (the first node or none)
//   proc.graph.loop             the edges form a loop: every node on it, in order
//   proc.graph.output           no output node, or more than one
//   proc.reference.surface      (check_volume_graph) a node reads a connection
//                               surface that doesn't join the volume: the graph
//                               would see past its own inputs (design rule 1)

#include <functional>

#include "vs/core/diagnostic.hpp"
#include "vs/model/document.hpp"
#include "vs/proc/graph.hpp"
#include "vs/proc/nodes.hpp"

namespace vs::proc {

core::Diagnostics check_graph(const Graph& graph, const NodeRegistry& registry = library());

// check_graph(), then that every surface a node reads joins `volume` in
// `document`.
core::Diagnostics check_volume_graph(const Graph& graph, const model::Document& document,
                                     const Id& volume, const NodeRegistry& registry = library());

// proc.graph.missing_input and proc.graph.output: the graph can't run yet,
// but nothing in it is wrong.
bool is_unfinished(std::string_view code);

// Applies `change` to a copy of `graph`; keeps it if the result has no
// errors but unfinished ones. Returns the result's diagnostics either way.
core::Diagnostics edit(Graph& graph, const std::function<void(Graph&)>& change,
                       const NodeRegistry& registry = library());

// edit() adding the edge `from` -> `to`.
core::Diagnostics connect(Graph& graph, const PortRef& from, const PortRef& to,
                          const NodeRegistry& registry = library());

// The nodes in evaluation order: every node after those feeding it, ties
// in the graph's node order. Empty when the graph has a loop.
std::vector<std::size_t> topological_order(const Graph& graph);

}  // namespace vs::proc

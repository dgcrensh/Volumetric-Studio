#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `nodes`: lists the node library: each type with its inputs, outputs and
// properties. Returns 0.
int run_nodes(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

// `graph <graph.json>`: reads a procedural graph, checks it (types, loops,
// settings, one output) and prints its nodes in evaluation order with what
// feeds each input. Returns 0 when it checks clean, 1 on errors (printed),
// 2 for bad arguments.
int run_graph(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vs::cli

#pragma once

// vs_proc: procedural node graphs (spec, "Procedural system"; plan,
// M10-M12). M10 is the runtime: the type system (types.hpp), fields and
// sampling (field.hpp, distance.hpp), the graph model and its JSON
// (graph.hpp), authoring checks (check.hpp), the node API and library
// (nodes.hpp), the graph's inputs (inputs.hpp), evaluation with a per-node
// cache (evaluate.hpp) and the engine's procedural generator
// (generator.hpp).

#include <string_view>

namespace vs::proc {

// The module's name, so the skeleton has a symbol to link and test.
std::string_view module_name();

}  // namespace vs::proc

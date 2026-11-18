#pragma once

// vs_proc: procedural node graph, type system, node library, fields and baking.
// Placeholder until M10; see Implementation Plan.md.

#include <string_view>

namespace vs::proc {

// The module's name, so the skeleton has a symbol to link and test.
std::string_view module_name();

}  // namespace vs::proc

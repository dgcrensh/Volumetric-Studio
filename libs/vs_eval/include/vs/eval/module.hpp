#pragma once

// vs_eval: scheduler, cache, thread pool, cancellation and volume states.
// Placeholder until M9; see Implementation Plan.md.

#include <string_view>

namespace vs::eval {

// The module's name, so the skeleton has a symbol to link and test.
std::string_view module_name();

}  // namespace vs::eval

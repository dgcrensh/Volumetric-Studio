#pragma once

// vs_eval: scheduler, cache, thread pool, cancellation and volume states.
// The engine is engine.hpp; see Implementation Plan.md (M9) and
// docs/decisions/0009.

#include <string_view>

namespace vs::eval {

// The module's name, so the skeleton has a symbol to link and test.
std::string_view module_name();

}  // namespace vs::eval

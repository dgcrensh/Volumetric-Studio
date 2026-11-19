#pragma once

// vs_sketch: sketch entities, constraints, the PlaneGCS wrapper and profile extraction.
// The module's API lives in sketch.hpp, solver.hpp, profile.hpp and serialize.hpp.

#include <string_view>

namespace vs::sketch {

// The module's name, so the skeleton has a symbol to link and test.
std::string_view module_name();

}  // namespace vs::sketch

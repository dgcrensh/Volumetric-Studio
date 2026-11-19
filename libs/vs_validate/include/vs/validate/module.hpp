#pragma once

// vs_validate: layout checks, containment and diagnostics routing.
// The checks are in layout.hpp, containment.hpp, registry.hpp and validate.hpp.

#include <string_view>

namespace vs::validate {

// The module's name, so the skeleton has a symbol to link and test.
std::string_view module_name();

}  // namespace vs::validate

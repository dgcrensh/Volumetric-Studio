#pragma once

// vs_kernel: kernel interface and its OCCT implementation (solids, booleans, STEP/IGES).
// Placeholder until M1; see Implementation Plan.md.

#include <string_view>

namespace vs::kernel {

// The module's name, so the skeleton has a symbol to link and test.
std::string_view module_name();

}  // namespace vs::kernel

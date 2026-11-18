#pragma once

// vs_mesh: mesh types, seam library, conforming tessellation, welding and mesh I/O.
// Placeholder until M7; see Implementation Plan.md.

#include <string_view>

namespace vs::mesh {

// The module's name, so the skeleton has a symbol to link and test.
std::string_view module_name();

}  // namespace vs::mesh

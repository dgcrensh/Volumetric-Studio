#pragma once

// vs_mesh: mesh types, seam library, conforming tessellation, welding and mesh I/O.
// Mesh types, topology checks, seams and welding are in the other headers
// (M7); see Implementation Plan.md and docs/decisions/0007.

#include <string_view>

namespace vs::mesh {

// The module's name, so the skeleton has a symbol to link and test.
std::string_view module_name();

}  // namespace vs::mesh

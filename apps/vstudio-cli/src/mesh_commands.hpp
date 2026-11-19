#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `seams <project>`: loads and builds a .vsproj, makes its seam library
// (vs_mesh, M7) and prints each connection surface's seam: spacing,
// outline and interior vertices, triangles and area. Returns 0 when every
// surface that needs a seam has one, 1 otherwise, 2 for bad arguments.
int run_seams(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vs::cli

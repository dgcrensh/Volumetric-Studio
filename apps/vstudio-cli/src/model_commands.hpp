#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `info <project> [--build]`: loads a .vsproj (folder or zip) and prints its
// volume tree, connection surfaces, keep-outs, links and dependencies. With
// --build it also builds the document and prints each volume's status, its
// boundary's volume and its published outputs. Returns 0 when the project
// loads (and, with --build, builds without errors), 1 otherwise.
int run_info(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vs::cli

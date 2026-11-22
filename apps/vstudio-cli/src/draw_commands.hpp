#pragma once

// Drawings (L6).

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `drawing <project> [<name>] [--standard [<name>]] [--save] [--svg <out>]
// [--pdf <out>] [--dxf <out>] [--config <name>] [--threads N]`: evaluates
// the project, makes the named drawing (the first without a name; a new
// standard one with --standard, kept with --save), prints each view and
// each dimension with its value, stable or disposable, then the
// diagnostics, and writes the sheet. Exits 1 on errors.
int run_drawing(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vs::cli

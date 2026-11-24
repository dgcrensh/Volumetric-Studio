#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `sketch-solve <in.json> [<out.json>]`: reads a sketch, solves it, prints
// the solve report and the profiles it closes, and writes the solved sketch
// when an output path is given. Returns 0 when the sketch solves (fully or
// under-constrained) and its profiles extract, 1 otherwise.
int run_sketch_solve(std::span<const std::string_view> args, std::ostream& out,
                     std::ostream& err);

// `sketch-import-svg <file.svg> [--out <sketch.json>] [--scale k] [--dpi n]
// [--gap mm] [--fix] [--no-smooth]`: imports an SVG file's outlines into a
// new sketch (S1), prints what came in, the warnings and the profiles it
// closes, and writes the sketch when asked. Returns 0 when the import works
// and every profile makes a valid face, 1 otherwise, 2 for bad arguments.
int run_sketch_import_svg(std::span<const std::string_view> args, std::ostream& out,
                          std::ostream& err);

}  // namespace vs::cli

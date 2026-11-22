#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `bench-rack [--cells N[,N...]] [--cell <mm>] [--threads N] [--csv <file>]`: the
// scaling benchmark of the preview tier. For each cell count (10, 100, 1,000 and
// 10,000 by default) builds the rack fixture at that size (a Base under a Rack
// composite whose children.grid generator makes nx * ny cells of `cell` mm, 8 by
// default) and runs Engine::preview() on a fresh engine, timing each phase (check,
// generated children, dependencies, layout, layout checks, keys and cache, ...) with
// its peak resident memory (Linux), then once more on the same engine. Prints a
// table per size, a summary and how time grows with the cell count; --csv writes
// every number. Returns 0, 1 if a preview reports errors, 2 for bad arguments.
int run_bench_rack(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vs::cli

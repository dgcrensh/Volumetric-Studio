#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `rebuild <project> [--threads N] [--check-determinism] [--weld] [--cache]
// [--set <volume>.<parameter>=<value>]... [--twice]`: evaluates the project with
// the M9 engine (procedural graphs through vs_proc's generator, M10) and prints each volume's status, cache key, whether it
// regenerated or came from the cache, its outputs, the run's statistics and
// its output hash. --cache keeps a disk cache in a folder project's cache/;
// --set changes parameters before evaluating; --twice evaluates a second
// time with the same engine (to show the cache at work). Returns 0 without
// errors, 1 with errors (a failed volume, nondeterminism), 2 for bad
// arguments.
int run_rebuild(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

// `bench-edit [--volumes N] [--repeat R] [--no-reuse] [--project <p>]`:
// times what an edit costs outside generation (History's check, the preview
// tier, the full evaluation) on rows of N parametric boxes with two links,
// for a contents edit, a parameter edit and an edit upstream of the links;
// with --project, a project's check, layout and evaluating it again.
// --no-reuse turns off the engine's layout reuse and checks the state
// before each edit too, as before decision 0029.
int run_bench_edit(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vs::cli

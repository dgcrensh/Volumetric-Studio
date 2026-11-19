#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `rebuild <project> [--threads N] [--check-determinism] [--weld] [--cache]
// [--set <volume>.<parameter>=<value>]... [--twice]`: evaluates the project with
// the M9 engine and prints each volume's status, cache key, whether it
// regenerated or came from the cache, its outputs, the run's statistics and
// its output hash. --cache keeps a disk cache in a folder project's cache/;
// --set changes parameters before evaluating; --twice evaluates a second
// time with the same engine (to show the cache at work). Returns 0 without
// errors, 1 with errors (a failed volume, nondeterminism), 2 for bad
// arguments.
int run_rebuild(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vs::cli

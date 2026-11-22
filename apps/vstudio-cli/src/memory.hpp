#pragma once

// The process's resident memory, for the benchmarks (bench-gyroid,
// bench-rack). Linux reads /proc/self/status; elsewhere nothing is
// measured and the functions return nullopt (reset does nothing).

#include <optional>

namespace vs::cli {

// Peak resident memory in MB since the process started or since the last
// reset_peak_rss() (VmHWM).
std::optional<double> peak_rss_mb();

// Resident memory in MB now (VmRSS).
std::optional<double> current_rss_mb();

// Starts a new peak at the current resident memory (clear_refs).
void reset_peak_rss();

}  // namespace vs::cli

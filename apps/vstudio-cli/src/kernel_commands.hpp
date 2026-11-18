#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `kernel-demo [<out.step>]`: builds the sample part, checks it, prints its
// properties and writes it as STEP (default: kernel-demo.step). Returns the
// process exit code.
int run_kernel_demo(std::span<const std::string_view> args, std::ostream& out,
                    std::ostream& err);

// `tolerance-spike`: runs the M1 tolerance spike and prints one row per case.
// Returns 0 when every case builds and is valid.
int run_tolerance_spike(std::span<const std::string_view> args, std::ostream& out,
                        std::ostream& err);

}  // namespace vs::cli

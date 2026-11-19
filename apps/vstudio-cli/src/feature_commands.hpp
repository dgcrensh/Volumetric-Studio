#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `build <in.json> [<out.step>]`: reads a feature tree, rebuilds it, prints
// the variables, each feature's status and diagnostics and the body's
// volume, and writes the body as STEP when an output path is given. Returns
// 0 when the rebuild has no errors, 1 otherwise.
int run_build(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vs::cli

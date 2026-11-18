#pragma once

#include <ostream>

namespace vs::cli {

// Runs quick built-in checks of the linked libraries and prints one line per
// check. Returns true when every check passes.
bool run_selftest(std::ostream& out);

}  // namespace vs::cli

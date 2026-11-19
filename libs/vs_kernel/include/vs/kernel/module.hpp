#pragma once

// vs_kernel: the kernel interface and its Open CASCADE implementation.
// Include "vs/kernel/kernel.hpp" for the whole API.

#include <string_view>

namespace vs::kernel {

// The module's name, used by the CLI's selftest to prove the module links.
std::string_view module_name();

}  // namespace vs::kernel

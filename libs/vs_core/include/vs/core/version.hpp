#pragma once

#include <string_view>

namespace vs::core {

// The studio version from the top-level project(), such as "0.1.0".
std::string_view version_string();

// The git commit the build came from (short hash, with "-dirty" when the
// working tree had local changes), or "unknown" outside a git checkout.
std::string_view git_revision();

// The engine version that goes into every cache key (spec: "Cache key").
// Bump it whenever generated output changes for identical inputs.
inline constexpr unsigned kEngineVersion = 1;

}  // namespace vs::core

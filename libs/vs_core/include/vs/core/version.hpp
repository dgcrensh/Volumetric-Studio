#pragma once

#include <string_view>

namespace vs::core {

// The studio version from the top-level project(), such as "0.1.0".
std::string_view version_string();

// The git commit the build came from (short hash, with "-dirty" when the
// working tree had local changes), or "unknown" outside a git checkout.
std::string_view git_revision();

// The engine version that goes into every cache key (spec: "Cache key").
// Bump it whenever generated output changes for identical inputs. 2 (H3):
// TriangleDistance's sorted BVH leaves and atan2_det(), and the
// ProductSettings in cache keys.
inline constexpr unsigned kEngineVersion = 2;

}  // namespace vs::core

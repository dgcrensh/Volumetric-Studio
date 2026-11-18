#pragma once

// Product settings (spec: "Geometry and precision").
//
// The working space and the linear tolerance are product settings, not values
// derived from machine epsilon. The defaults are the spec's proposed values:
// a ±1,000 mm box and an absolute tolerance of 1e-6 mm. M1's tolerance spike
// tests whether OCCT copes with them and may change the defaults.

#include "vs/core/box.hpp"
#include "vs/core/diagnostic.hpp"
#include "vs/core/hash.hpp"
#include "vs/core/vec.hpp"

namespace vs::core {

struct ProductSettings {
  // The smallest allowed feature is this many times the tolerance.
  static constexpr double kMinFeatureFactor = 1000.0;

  // Half the edge length of the cubic model space, centred on the origin.
  double working_half_extent_mm = 1000.0;
  // Absolute linear tolerance for touching and containment checks.
  double linear_tolerance_mm = 1e-6;

  static constexpr ProductSettings defaults() { return {}; }

  [[nodiscard]] constexpr double min_feature_size_mm() const {
    return linear_tolerance_mm * kMinFeatureFactor;
  }

  [[nodiscard]] constexpr dbox3 working_box() const {
    const double e = working_half_extent_mm;
    return dbox3::from_min_max({-e, -e, -e}, {e, e, e});
  }

  // True when `p` lies inside the working space (boundary included).
  [[nodiscard]] constexpr bool in_working_space(const dvec3& p) const {
    return working_box().contains(p);
  }

  // Errors for settings the studio can't work with: non-finite or
  // non-positive values, or a tolerance too coarse for the working space.
  [[nodiscard]] Diagnostics validate() const;

  friend constexpr bool operator==(const ProductSettings&, const ProductSettings&) = default;
};

void hash_append(Hasher& h, const ProductSettings& s);

}  // namespace vs::core

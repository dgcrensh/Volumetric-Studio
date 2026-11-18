#include "vs/core/settings.hpp"

#include <cmath>

#include <fmt/format.h>

namespace vs::core {

namespace {

SubjectRef setting(std::string_view name) {
  return SubjectRef{SubjectKind::Setting, Id::null(), std::string(name)};
}

}  // namespace

Diagnostics ProductSettings::validate() const {
  Diagnostics out;

  if (!std::isfinite(working_half_extent_mm) || working_half_extent_mm <= 0.0) {
    out.add(Diagnostic::error(
        "core.settings.working_space",
        fmt::format("Working-space half extent must be a positive number of millimetres, "
                    "not {}",
                    working_half_extent_mm),
        {setting("working_half_extent_mm")}));
  }

  if (!std::isfinite(linear_tolerance_mm) || linear_tolerance_mm <= 0.0) {
    out.add(Diagnostic::error(
        "core.settings.tolerance",
        fmt::format("Linear tolerance must be a positive number of millimetres, not {}",
                    linear_tolerance_mm),
        {setting("linear_tolerance_mm")}));
  }

  if (out.empty() && min_feature_size_mm() >= working_half_extent_mm) {
    out.add(Diagnostic::error(
        "core.settings.tolerance_too_coarse",
        fmt::format("The smallest allowed feature ({} mm, {}x the tolerance) does not fit "
                    "in the working space (half extent {} mm)",
                    min_feature_size_mm(), kMinFeatureFactor, working_half_extent_mm),
        {setting("linear_tolerance_mm"), setting("working_half_extent_mm")}));
  }

  // FP64 has a 53-bit mantissa. At the far edge of the working space the
  // spacing between adjacent doubles must stay well below the tolerance, or
  // the tolerance can't be represented there at all.
  if (out.empty()) {
    const double ulp_at_edge =
        std::nextafter(working_half_extent_mm, INFINITY) - working_half_extent_mm;
    if (ulp_at_edge * 1000.0 > linear_tolerance_mm) {
      out.add(Diagnostic::error(
          "core.settings.tolerance_below_precision",
          fmt::format("A tolerance of {} mm is too fine for FP64 at {} mm from the origin "
                      "(adjacent doubles are {} mm apart there)",
                      linear_tolerance_mm, working_half_extent_mm, ulp_at_edge),
          {setting("linear_tolerance_mm"), setting("working_half_extent_mm")}));
    }
  }

  return out;
}

void hash_append(Hasher& h, const ProductSettings& s) {
  h.add(std::string_view("vs.core.ProductSettings.v1"))
      .add(s.working_half_extent_mm)
      .add(s.linear_tolerance_mm);
}

}  // namespace vs::core

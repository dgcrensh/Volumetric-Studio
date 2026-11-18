#pragma once

// The M1 tolerance spike: does Open CASCADE cope with the product's working
// space and linear tolerance?
//
// Each case builds a part near the edge of the working space (or at the
// origin, as a baseline), runs the booleans a real model would, and records
// whether the result is valid, how far its volume is from the analytic value,
// and how far the kernel's per-entity tolerances grew. The test suite asserts
// on these numbers, and `vstudio-cli tolerance-spike` prints them; the
// findings are recorded in docs/decisions/0001-working-space-and-tolerance.md.

#include <string>
#include <vector>

#include "vs/core/settings.hpp"
#include "vs/kernel/check.hpp"

namespace vs::kernel {

struct SpikeCase {
  std::string name;
  std::string description;
  bool built = false;          // every operation succeeded
  bool valid = false;          // the result passed the validity check
  std::size_t solids = 0;      // lumps in the result
  double expected_volume = 0;  // analytic, mm³
  double volume = 0;           // measured, mm³
  ToleranceAudit audit;
  Diagnostics diagnostics;     // why it failed to build, if it did

  [[nodiscard]] double volume_relative_error() const;
};

std::vector<SpikeCase> run_tolerance_spike(const core::ProductSettings& settings);

}  // namespace vs::kernel

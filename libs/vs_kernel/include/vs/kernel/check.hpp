#pragma once

// Validity and tolerance checks.
//
// Open CASCADE stores a tolerance on every vertex, edge and face, and
// operations (booleans especially) can grow them. The spec asks that these be
// checked against the studio's global tolerance after each feature. The audit
// here does that and reports the worst offenders.

#include <cstddef>

#include "vs/core/settings.hpp"
#include "vs/kernel/shape.hpp"

namespace vs::kernel {

// Runs the kernel's full validity check. Empty when the shape is valid;
// otherwise one "kernel.check.invalid" error that names the failing checks.
Diagnostics check_validity(const Shape& shape);

struct ToleranceStats {
  std::size_t count = 0;     // entities of this kind
  std::size_t exceeding = 0; // entities whose tolerance is above the limit
  double max = 0.0;          // largest tolerance, mm
};

struct ToleranceAudit {
  double limit_mm = 0.0;  // the product's linear tolerance
  ToleranceStats vertices;
  ToleranceStats edges;
  ToleranceStats faces;

  [[nodiscard]] double max_tolerance() const;
  [[nodiscard]] std::size_t exceeding() const {
    return vertices.exceeding + edges.exceeding + faces.exceeding;
  }
  [[nodiscard]] bool within_limit() const { return exceeding() == 0; }

  // One "kernel.tolerance.exceeded" warning per entity kind that has
  // entities above the limit, or nothing when all are within it.
  [[nodiscard]] Diagnostics diagnostics() const;
};

// Measures every vertex, edge and face tolerance of `shape` against
// settings.linear_tolerance_mm.
ToleranceAudit audit_tolerances(const Shape& shape, const core::ProductSettings& settings);

}  // namespace vs::kernel

#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/kernel.hpp"

using namespace vs::kernel;
using vs::core::dbox3;
using vs::core::ProductSettings;

namespace {
const ProductSettings kSettings = ProductSettings::defaults();
}

TEST_CASE("valid shapes pass the validity check", "[kernel][check]") {
  const auto part = make_demo_part(kSettings);
  REQUIRE(part.ok());
  CHECK(check_validity(*part).empty());
  CHECK(check_validity(Shape{}).empty());
}

TEST_CASE("the tolerance audit counts every entity", "[kernel][check]") {
  const auto b = make_box(dbox3::from_min_max({0, 0, 0}, {1, 2, 3}), kSettings);
  REQUIRE(b.ok());
  const ToleranceAudit audit = audit_tolerances(*b, kSettings);
  CHECK(audit.limit_mm == kSettings.linear_tolerance_mm);
  CHECK(audit.vertices.count == 8);
  CHECK(audit.edges.count == 12);
  CHECK(audit.faces.count == 6);
  // A fresh primitive carries the kernel's default tolerance (1e-7 mm),
  // inside the product tolerance.
  CHECK(audit.max_tolerance() > 0.0);
  CHECK(audit.max_tolerance() <= kSettings.linear_tolerance_mm);
  CHECK(audit.within_limit());
  CHECK(audit.diagnostics().empty());
}

TEST_CASE("the tolerance audit flags entities above the product tolerance", "[kernel][check]") {
  // A product tolerance tighter than the kernel's default makes every entity
  // exceed it.
  ProductSettings tight = kSettings;
  tight.linear_tolerance_mm = 1e-9;
  const auto b = make_box(dbox3::from_min_max({0, 0, 0}, {1, 2, 3}), kSettings);
  REQUIRE(b.ok());
  const ToleranceAudit audit = audit_tolerances(*b, tight);
  CHECK_FALSE(audit.within_limit());
  CHECK(audit.exceeding() == 8 + 12 + 6);
  const Diagnostics d = audit.diagnostics();
  REQUIRE(d.size() == 3);
  CHECK(d.with_code("kernel.tolerance.exceeded").size() == 3);
  CHECK_FALSE(d.has_errors());  // a warning: the shape is still usable
}

#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "vs/core/settings.hpp"

using namespace vs::core;

TEST_CASE("default settings are the spec's proposed values", "[settings]") {
  const auto s = ProductSettings::defaults();
  CHECK(s.working_half_extent_mm == 1000.0);
  CHECK(s.linear_tolerance_mm == 1e-6);
  CHECK(s.min_feature_size_mm() == 1e-3);
  CHECK(s.validate().empty());
}

TEST_CASE("working space", "[settings]") {
  const auto s = ProductSettings::defaults();
  CHECK(s.working_box() == dbox3::from_min_max({-1000, -1000, -1000}, {1000, 1000, 1000}));
  CHECK(s.in_working_space({1000, -1000, 0}));
  CHECK_FALSE(s.in_working_space({1000.001, 0, 0}));
}

TEST_CASE("invalid settings produce named diagnostics", "[settings]") {
  ProductSettings s;
  s.linear_tolerance_mm = 0.0;
  auto d = s.validate();
  REQUIRE(d.size() == 1);
  CHECK(d[0].code == "core.settings.tolerance");
  REQUIRE(d[0].target() != nullptr);
  CHECK(d[0].target()->kind == SubjectKind::Setting);
  CHECK(d[0].target()->label == "linear_tolerance_mm");

  s = ProductSettings{};
  s.working_half_extent_mm = std::numeric_limits<double>::quiet_NaN();
  d = s.validate();
  REQUIRE(d.size() == 1);
  CHECK(d[0].code == "core.settings.working_space");

  s = ProductSettings{};
  s.linear_tolerance_mm = 1.0;  // smallest feature would be 1,000 mm
  d = s.validate();
  REQUIRE(d.size() == 1);
  CHECK(d[0].code == "core.settings.tolerance_too_coarse");

  s = ProductSettings{};
  s.linear_tolerance_mm = 1e-12;  // below what FP64 resolves at 1,000 mm
  d = s.validate();
  REQUIRE(d.size() == 1);
  CHECK(d[0].code == "core.settings.tolerance_below_precision");
}

TEST_CASE("settings hash by value", "[settings][hash]") {
  ProductSettings a;
  ProductSettings b;
  CHECK(hash_of(a) == hash_of(b));
  b.linear_tolerance_mm = 1e-5;
  CHECK(hash_of(a) != hash_of(b));
}

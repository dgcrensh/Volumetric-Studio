// H2: rebuild bypasses that used to pass. Each case failed before H2.

#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "parts.hpp"

using namespace vs::feature;
using Catch::Approx;
using parts::kSettings;

TEST_CASE("An edit above the start feature isn't hidden by reuse", "[feature][rebuild][h2]") {
  auto plate = parts::make_plate(60, 8, 12);
  const auto full = rebuild(plate.tree, kSettings);
  REQUIRE(full.ok());
  const double hole_area = std::numbers::pi * 36;

  SECTION("a changed definition") {
    // The plate's depth set to a number: the variables are as they were,
    // but the plate isn't. Rebuilding from the hole (a caller's mistake,
    // or a stale start) must not reuse the old plate.
    plate.tree.find(plate.base)->as<ExtrudeFeature>()->depth = Param::of(5);
    const auto partial = rebuild(plate.tree, kSettings, {&full, plate.hole});
    REQUIRE(partial.ok());
    CHECK_FALSE(partial.find(plate.base)->reused);
    CHECK(parts::volume(partial.body) == Approx(60 * 30 * 5 - hole_area * 5).epsilon(1e-9));
  }
  SECTION("a changed sketch") {
    auto* sketch = plate.tree.sketch(plate.base_sketch);
    REQUIRE(sketch != nullptr);
    sketch->set_name("Renamed outline");
    const auto partial = rebuild(plate.tree, kSettings, {&full, plate.hole});
    CHECK_FALSE(partial.find(plate.base_sketch)->reused);
  }
  SECTION("other settings") {
    auto finer = kSettings;
    finer.linear_tolerance_mm = 1e-7;
    const auto partial = rebuild(plate.tree, finer, {&full, plate.hole});
    CHECK_FALSE(partial.find(plate.base)->reused);
  }
  SECTION("nothing changed: reused as before") {
    const auto partial = rebuild(plate.tree, kSettings, {&full, plate.hole});
    CHECK(partial.find(plate.base)->reused);
    CHECK(partial.rebuilt_count() == 1);
  }
}

TEST_CASE("An input value never stands in for a failed tree variable",
          "[feature][external][h2]") {
  FeatureTree tree;
  vs::sketch::Sketch s(Id::random(), "Outline");
  const auto r = parts::add_rectangle(s, {0, 0}, {10, 20});
  parts::constrain_rectangle(s, r);
  const Id sketch = tree.add(SketchFeature{s, PrincipalPlane::XY});
  ExtrudeFeature e;
  e.sketch = sketch;
  e.depth = Param::expr("depth");
  tree.add(e, "Block");
  // The tree's own `depth` doesn't evaluate (a division by zero).
  REQUIRE(tree.variables().add("depth", "1 mm / 0").ok());

  ExternalInputs inputs;
  inputs.values["depth"] = 3.0;
  RebuildOptions options;
  options.inputs = &inputs;
  const auto result = rebuild(tree, kSettings, options);
  INFO(parts::codes(result.diagnostics));
  CHECK_FALSE(result.ok());
  CHECK_FALSE(result.variables.get("depth").has_value());
  CHECK(result.body.is_empty());
}

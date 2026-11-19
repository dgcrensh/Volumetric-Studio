// The M3 acceptance items from the implementation plan.

#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "parts.hpp"
#include "vs/feature/rebuild.hpp"
#include "vs/kernel/check.hpp"
#include "vs/kernel/queries.hpp"

using namespace vs::feature;
using Catch::Approx;
using parts::kSettings;
using vs::core::dvec2;

TEST_CASE("a part driven by three variables rebuilds when each one changes",
          "[feature][acceptance]") {
  auto plate = parts::make_plate(60, 8, 12);
  auto& vars = plate.tree.variables();

  auto check = [&](double w, double t, double d) {
    const auto result = rebuild(plate.tree, kSettings);
    INFO(parts::codes(result.diagnostics));
    REQUIRE(result.ok());
    CHECK(vs::kernel::check_validity(result.body).empty());
    CHECK(result.body.solid_count() == 1);
    CHECK(parts::volume(result.body) == Approx(parts::plate_volume(w, t, d)).epsilon(1e-9));
    const auto box = vs::kernel::bounding_box(result.body);
    CHECK(box.min.x == Approx(0).margin(1e-6));
    CHECK(box.max.x == Approx(w).epsilon(1e-9));
    CHECK(box.max.y == Approx(w / 2).epsilon(1e-9));
    CHECK(box.max.z == Approx(t).epsilon(1e-9));
  };

  check(60, 8, 12);
  // The width drives two sketch dimensions in the base and the hole's
  // position in the second sketch.
  REQUIRE(vars.set_expression(plate.width, "80 mm").empty());
  check(80, 8, 12);
  // The thickness moves the top face the hole sketch sits on.
  REQUIRE(vars.set_expression(plate.thickness, "0.5 in").empty());
  check(80, 12.7, 12);
  // The hole diameter is a sketch dimension on that face.
  REQUIRE(vars.set_expression(plate.hole_d, "thickness * 2").empty());
  check(80, 12.7, 25.4);
}

TEST_CASE("an equation loop is rejected when authored, with the loop as a path",
          "[feature][acceptance]") {
  VariableTable vars;
  const Id a = vars.add("a", "10").value();
  const Id b = vars.add("b", "a * 2").value();
  const Id c = vars.add("c", "b + 1").value();

  // a → c closes a → c → b → a.
  const auto problems = vars.set_expression(a, "c / 3");
  REQUIRE(problems.size() == 1);
  const auto& d = problems[0];
  CHECK(d.code == "feature.variable.loop");
  CHECK(d.message.find("a \xE2\x86\x92 c \xE2\x86\x92 b \xE2\x86\x92 a") != std::string::npos);
  REQUIRE(d.subjects.size() == 3);
  CHECK(d.subjects[0].id == a);  // the one being edited: act here
  CHECK(d.subjects[1].id == c);
  CHECK(d.subjects[2].id == b);

  // The table is unchanged and still evaluates.
  CHECK(vars.find(a)->expression == "10");
  const auto values = vars.evaluate();
  CHECK(values.diagnostics.empty());
  CHECK(*values.get("c") == 21.0);

  // A variable that uses itself is a loop of one.
  const auto self = vars.add("d", "d + 1");
  REQUIRE_FALSE(self.ok());
  CHECK(self.error().code == "feature.variable.loop");
  CHECK(vars.find("d") == nullptr);
}

TEST_CASE("deleting a referenced sketch entity breaks only the feature that used it",
          "[feature][acceptance]") {
  FeatureTree tree;
  vs::sketch::Sketch base;
  parts::add_rectangle(base, {0, 0}, {40, 30});
  const Id base_id = tree.add(SketchFeature{base});
  ExtrudeFeature plate;
  plate.sketch = base_id;
  plate.depth = Param::of(10);
  const Id plate_id = tree.add(plate);

  // A groove: a 10 x 2 rectangle on the top face, revolved about a
  // construction line along its long edge.
  vs::sketch::Sketch groove;
  parts::add_rectangle(groove, {5, 15}, {15, 17});
  const Id axis_line = groove.add_line(dvec2{0, 15}, dvec2{40, 15}, /*construction=*/true);
  const Id groove_sketch = tree.add(SketchFeature{groove, FaceRef{plate_id, "end"}});
  RevolveFeature revolve;
  revolve.sketch = groove_sketch;
  revolve.operation = Operation::Cut;
  revolve.axis = SketchEntityRef{groove_sketch, axis_line};
  const Id revolve_id = tree.add(revolve, "Groove");
  // Something independent after it.
  const Id later = tree.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(50)}});

  const auto before = rebuild(tree, kSettings);
  INFO(parts::codes(before.diagnostics));
  REQUIRE(before.ok());
  const double plate_volume = 40.0 * 30.0 * 10.0;
  // Half of a 2 mm radius, 10 mm long cylinder lies inside the plate.
  CHECK(parts::volume(before.body) ==
        Approx(plate_volume - std::numbers::pi * 4.0 * 10.0 / 2.0).epsilon(1e-9));

  REQUIRE(tree.sketch(groove_sketch)->remove_entity(axis_line));
  const auto after = rebuild(tree, kSettings);

  REQUIRE(after.diagnostics.has_errors());
  const auto broken = after.diagnostics.with_code("feature.reference.broken");
  REQUIRE(broken.size() == 1);
  REQUIRE(broken[0]->target() != nullptr);
  CHECK(broken[0]->target()->id == revolve_id);
  CHECK(broken[0]->target()->kind == vs::core::SubjectKind::Feature);
  CHECK(broken[0]->message.find("Groove") != std::string::npos);
  // Exactly one error, on the right feature.
  CHECK(after.diagnostics.count(vs::core::Severity::Error) == 1);

  // Everything before it still rebuilds, and so does what follows.
  CHECK(after.find(base_id)->ok());
  CHECK(after.find(plate_id)->ok());
  CHECK(after.find(groove_sketch)->ok());
  CHECK(after.find(revolve_id)->status == FeatureStatus::Failed);
  CHECK(after.find(later)->ok());
  CHECK(parts::volume(after.body) == Approx(plate_volume).epsilon(1e-9));
}

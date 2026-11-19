#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "parts.hpp"
#include "vs/feature/rebuild.hpp"

using namespace vs::feature;
using Catch::Approx;
using parts::kSettings;
using vs::sketch::Sketch;

namespace {

const Diagnostic& only_error(const RebuildResult& r, const Id& feature) {
  const auto* f = r.find(feature);
  REQUIRE(f != nullptr);
  INFO(parts::codes(f->diagnostics));
  REQUIRE(f->status == FeatureStatus::Failed);
  REQUIRE(f->diagnostics.count(vs::core::Severity::Error) >= 1);
  REQUIRE(f->diagnostics[0].target() != nullptr);
  CHECK(f->diagnostics[0].target()->id == feature);
  return f->diagnostics[0];
}

}  // namespace

TEST_CASE("suppressed features are skipped", "[feature][rebuild]") {
  auto plate = parts::make_plate(60, 8, 12);
  REQUIRE(plate.tree.set_suppressed(plate.hole, true));
  auto r = rebuild(plate.tree, kSettings);
  REQUIRE(r.ok());
  CHECK(r.find(plate.hole)->status == FeatureStatus::Suppressed);
  CHECK(parts::volume(r.body) == Approx(60 * 30 * 8).epsilon(1e-12));

  // Suppressing the hole's sketch fails the hole, which used it.
  REQUIRE(plate.tree.set_suppressed(plate.hole, false));
  REQUIRE(plate.tree.set_suppressed(plate.hole_sketch, true));
  r = rebuild(plate.tree, kSettings);
  CHECK(only_error(r, plate.hole).code == "feature.reference.suppressed");
  CHECK(parts::volume(r.body) == Approx(60 * 30 * 8).epsilon(1e-12));
}

TEST_CASE("rebuilding from a changed feature reuses what is above it", "[feature][rebuild]") {
  auto plate = parts::make_plate(60, 8, 12);
  const auto full = rebuild(plate.tree, kSettings);
  REQUIRE(full.ok());
  CHECK(full.rebuilt_count() == 4);

  // Change only the last feature: the three above are reused.
  plate.tree.find(plate.hole)->as<ExtrudeFeature>()->extent = Extent::Blind;
  plate.tree.find(plate.hole)->as<ExtrudeFeature>()->depth = Param::of(3);
  const auto partial = rebuild(plate.tree, kSettings, {&full, plate.hole});
  REQUIRE(partial.ok());
  CHECK(partial.rebuilt_count() == 1);
  CHECK(partial.find(plate.base)->reused);
  CHECK_FALSE(partial.find(plate.hole)->reused);
  const double hole_area = std::numbers::pi * 36;
  CHECK(parts::volume(partial.body) == Approx(60 * 30 * 8 - hole_area * 3).epsilon(1e-9));
  // ...and it matches a full rebuild.
  const auto again = rebuild(plate.tree, kSettings);
  CHECK(parts::volume(again.body) == Approx(parts::volume(partial.body)).epsilon(1e-12));

  // A changed variable invalidates everything, whatever the start.
  REQUIRE(plate.tree.variables().set_expression(plate.width, "70").empty());
  const auto after_variable = rebuild(plate.tree, kSettings, {&partial, plate.hole});
  CHECK(after_variable.rebuilt_count() == 4);
  CHECK(parts::volume(after_variable.body) ==
        Approx(70 * 35 * 8 - hole_area * 3).epsilon(1e-9));

  // So does a tree whose order above the start changed.
  REQUIRE(plate.tree.move(plate.hole_sketch, 0));
  const auto reordered = rebuild(plate.tree, kSettings, {&after_variable, plate.hole});
  CHECK(reordered.rebuilt_count() == 4);
}

TEST_CASE("references must point upwards and at something that still exists",
          "[feature][rebuild]") {
  auto plate = parts::make_plate();
  // Moving the hole above its sketch makes the reference point forwards.
  REQUIRE(plate.tree.move(plate.hole, 2));
  auto r = rebuild(plate.tree, kSettings);
  CHECK(only_error(r, plate.hole).code == "feature.reference.forward");
  CHECK(only_error(r, plate.hole).subjects[1].id == plate.hole_sketch);
  REQUIRE(plate.tree.move(plate.hole, 3));

  // Removing the base extrude breaks the hole sketch's face reference, and
  // the hole fails with it.
  REQUIRE(plate.tree.remove(plate.base));
  r = rebuild(plate.tree, kSettings);
  CHECK(only_error(r, plate.hole_sketch).code == "feature.reference.broken");
  CHECK(only_error(r, plate.hole).code == "feature.reference.upstream_failed");
  CHECK(r.find(plate.base_sketch)->ok());
  CHECK(r.body.is_empty());
}

TEST_CASE("a face reference breaks when a later feature removes the face",
          "[feature][rebuild]") {
  auto plate = parts::make_plate(60, 8, 12);
  // Skim the whole top off: a cut over the full plate, 1 mm deep, from a
  // sketch on the top face.
  Sketch skim;
  parts::add_rectangle(skim, {-1, -1}, {61, 31});
  const Id skim_sketch = plate.tree.insert(
      2, Feature{Id::random(), "Skim sketch", false, SketchFeature{skim, FaceRef{plate.base, "end"}}});
  ExtrudeFeature skim_cut;
  skim_cut.sketch = skim_sketch;
  skim_cut.operation = Operation::Cut;
  skim_cut.depth = Param::of(1);
  plate.tree.insert(3, Feature{Id::random(), "Skim", false, skim_cut});

  const auto r = rebuild(plate.tree, kSettings);
  const auto& d = only_error(r, plate.hole_sketch);
  CHECK(d.code == "feature.reference.broken");
  CHECK(d.message.find("no longer on the body") != std::string::npos);
  CHECK(d.subjects[1].id == plate.base);
  CHECK(only_error(r, plate.hole).code == "feature.reference.upstream_failed");
  CHECK(parts::volume(r.body) == Approx(60 * 30 * 7).epsilon(1e-9));
}

TEST_CASE("a side face follows its sketch line; deleting the line breaks it",
          "[feature][rebuild]") {
  FeatureTree tree;
  Sketch base;
  const auto rect = parts::add_rectangle(base, {0, 0}, {20, 10});
  // A construction line closes nothing, but deleting the right-hand line
  // and replacing it with an arc keeps a closed profile without that line.
  const Id base_id = tree.add(SketchFeature{base});
  ExtrudeFeature block;
  block.sketch = base_id;
  block.depth = Param::of(5);
  const Id block_id = tree.add(block);
  const std::string right = "side:" + rect.l[1].to_string();
  const Id on_side = tree.add(DatumPlaneFeature{PlaneOffset{FaceRef{block_id, right}, Param::of(2)}});

  auto r = rebuild(tree, kSettings);
  REQUIRE(r.ok());
  CHECK(r.find(on_side)->plane->origin.x == Approx(22));

  // Swap the right-hand line for an arc bulging out to x = 25.
  auto* sk = tree.sketch(base_id);
  REQUIRE(sk->remove_entity(rect.l[1]));
  const Id centre = sk->add_point({20, 5});
  sk->add_arc(centre, rect.p[1], rect.p[2]);
  r = rebuild(tree, kSettings);
  CHECK(r.find(block_id)->ok());
  const auto& d = only_error(r, on_side);
  CHECK(d.code == "feature.reference.broken");
  CHECK(d.message.find("sketch line") != std::string::npos);
}

TEST_CASE("parameter expressions report bad variables on the feature", "[feature][rebuild]") {
  auto plate = parts::make_plate();
  plate.tree.find(plate.base)->as<ExtrudeFeature>()->depth = Param::expr("depth_missing");
  auto r = rebuild(plate.tree, kSettings);
  CHECK(only_error(r, plate.base).code == "feature.variable.unknown");

  plate.tree.find(plate.base)->as<ExtrudeFeature>()->depth = Param::expr("thickness / 0");
  r = rebuild(plate.tree, kSettings);
  CHECK(only_error(r, plate.base).code == "feature.expression.not_finite");

  // A sketch dimension naming a missing variable fails the sketch, and the
  // constraint is the second subject.
  plate.tree.find(plate.base)->as<ExtrudeFeature>()->depth = Param::expr("thickness");
  auto* sk = plate.tree.sketch(plate.base_sketch);
  std::vector<Id> width_dimensions;
  for (const auto& c : sk->constraints()) {
    if (c.dimension && c.dimension->expression == "width") width_dimensions.push_back(c.id);
  }
  REQUIRE(width_dimensions.size() == 1);
  parts::set_expression(*sk, width_dimensions[0], "breadth");
  r = rebuild(plate.tree, kSettings);
  const auto& d = only_error(r, plate.base_sketch);
  CHECK(d.code == "feature.variable.unknown");
  CHECK(d.subjects[1].kind == vs::core::SubjectKind::Constraint);
}

TEST_CASE("a sketch that doesn't solve fails its feature", "[feature][rebuild]") {
  FeatureTree tree;
  Sketch s;
  const auto rect = parts::add_rectangle(s, {0, 0}, {10, 10});
  s.length(rect.l[0], 10);
  s.length(rect.l[0], 12);  // conflicts
  s.horizontal(rect.l[0]);
  const Id id = tree.add(SketchFeature{s});
  const auto r = rebuild(tree, kSettings);
  const auto& d = only_error(r, id);
  CHECK(d.code == "feature.sketch.unsolved");
  CHECK_FALSE(r.find(id)->diagnostics.with_code("sketch.solve.conflicting").empty());
}

TEST_CASE("labels use the name, the sketch's name, or kind and count", "[feature][tree]") {
  auto plate = parts::make_plate();
  CHECK(plate.tree.label(plate.base_sketch) == "Base");  // the sketch's own name
  CHECK(plate.tree.label(plate.base) == "Plate");
  CHECK(plate.tree.label(plate.hole_sketch) == "Hole");
  const Id unnamed = plate.tree.add(DatumPlaneFeature{PlaneOffset{}});
  const Id unnamed2 = plate.tree.add(DatumPlaneFeature{PlaneOffset{}});
  CHECK(plate.tree.label(unnamed) == "Plane 1");
  CHECK(plate.tree.label(unnamed2) == "Plane 2");
  CHECK(plate.tree.subject(unnamed).kind == vs::core::SubjectKind::Feature);
  CHECK(plate.tree.validate().empty());
}

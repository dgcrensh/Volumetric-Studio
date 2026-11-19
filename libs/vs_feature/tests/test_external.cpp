// references_of() and external inputs (M5: what vs_model needs from a tree).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "parts.hpp"

using namespace vs::feature;
using Catch::Approx;
using parts::kSettings;

TEST_CASE("references_of lists what a feature uses", "[feature][references]") {
  auto plate = parts::make_plate();
  const auto& tree = plate.tree;

  const auto base = references_of(*tree.find(plate.base));
  CHECK(base.features == std::vector<Id>{plate.base_sketch});
  CHECK(base.variables == std::vector<std::string>{"thickness"});

  // The base sketch's dimensions use width (twice: width and width / 2).
  const auto sketch = references_of(*tree.find(plate.base_sketch));
  CHECK(sketch.features.empty());
  CHECK(sketch.variables == std::vector<std::string>{"width"});

  // The hole sketch sits on a face of the plate.
  const auto hole_sketch = references_of(*tree.find(plate.hole_sketch));
  CHECK(hole_sketch.features == std::vector<Id>{plate.base});

  FilletFeature f;
  f.edges.push_back({FaceRef{plate.base, "end"}, FaceRef{plate.hole, "side:x"}});
  f.radius = Param::expr("hole_d / 10 + thickness");
  const Feature fillet{Id::random(), "", false, f};
  const auto fr = references_of(fillet);
  CHECK(fr.features == std::vector<Id>{plate.base, plate.hole});
  CHECK(fr.variables == std::vector<std::string>{"hole_d", "thickness"});
}

TEST_CASE("External inputs stand in for features outside the tree", "[feature][external]") {
  // A tree that sketches on an input plane and extrudes by an input value.
  FeatureTree tree;
  const Id outside = Id::random();
  vs::sketch::Sketch s(Id::random(), "On the input");
  const auto r = parts::add_rectangle(s, {0, 0}, {10, 20});
  parts::constrain_rectangle(s, r);
  const Id sketch = tree.add(SketchFeature{s, FaceRef{outside, "top"}});
  ExtrudeFeature e;
  e.sketch = sketch;
  e.depth = Param::expr("depth");
  tree.add(e, "Block");

  ExternalInputs inputs;
  ExternalInput in;
  in.subject = {vs::core::SubjectKind::Boundary, outside, "boundary of 'X'"};
  in.result.faces.push_back({"top", {{0, 0, 5}, {0, 0, 1}}, std::nullopt});
  inputs.references.push_back(in);
  inputs.values["depth"] = 3.0;

  RebuildOptions options;
  options.inputs = &inputs;
  const auto result = rebuild(tree, kSettings, options);
  INFO(parts::codes(result.diagnostics));
  REQUIRE(result.ok());
  CHECK(parts::volume(result.body) == Approx(10.0 * 20 * 3));
  CHECK(result.features[0].sketch->plane().origin.z == Approx(5));

  SECTION("without the inputs the references are broken") {
    const auto bare = rebuild(tree, kSettings);
    CHECK(bare.diagnostics.with_code("feature.reference.broken").size() == 1);
  }
  SECTION("a missing face of an input is broken, naming the input") {
    inputs.references[0].result.faces[0].name = "bottom";
    const auto broken = rebuild(tree, kSettings, options);
    const auto found = broken.diagnostics.with_code("feature.reference.broken");
    REQUIRE(found.size() == 1);
    CHECK(found[0]->subjects.at(1).id == outside);
  }
  SECTION("a variable of the tree wins over an input value") {
    REQUIRE(tree.variables().add("depth", "4").ok());
    const auto mine = rebuild(tree, kSettings, options);
    REQUIRE(mine.ok());
    CHECK(parts::volume(mine.body) == Approx(10.0 * 20 * 4));
  }
}

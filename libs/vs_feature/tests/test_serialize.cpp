#include <filesystem>
#include <fstream>
#include <sstream>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "parts.hpp"
#include "vs/feature/rebuild.hpp"
#include "vs/feature/serialize.hpp"

using namespace vs::feature;
using Catch::Approx;
using parts::kSettings;

namespace {

const std::filesystem::path kData = VS_FEATURE_TEST_DATA;

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// A tree with every kind of feature and reference.
FeatureTree everything() {
  auto plate = parts::make_plate();
  FeatureTree& t = plate.tree;
  const Id p1 = t.add(DatumPointFeature{PointAt{Param::of(0), Param::expr("width"), Param::of(1)}},
                      "P1");
  const Id p2 = t.add(DatumPointFeature{PointAtRef{SketchEntityRef{plate.base_sketch, plate.rect.p[2]}}});
  const Id p3 = t.add(DatumPointFeature{PointAxisPlane{PrincipalAxis::Z, FaceRef{plate.base, "end"}}});
  t.add(DatumPlaneFeature{PlaneThroughPoints{FeatureRef{p1}, FeatureRef{p2}, FeatureRef{p3}}});
  const Id ax = t.add(DatumAxisFeature{AxisThroughPoints{FeatureRef{p1}, FeatureRef{p3}}});
  t.add(DatumAxisFeature{AxisPlaneIntersection{PrincipalPlane::XY, PrincipalPlane::ZX}});
  t.add(DatumAxisFeature{AxisAlong{SketchEntityRef{plate.base_sketch, plate.rect.l[0]}}});
  t.add(DatumPlaneFeature{PlaneAxisAngle{FeatureRef{ax}, PrincipalPlane::YZ, Param::expr("30 deg")}});
  const Id off = t.add(DatumPlaneFeature{PlaneOffset{FaceRef{plate.base, "start"}, Param::of(-2.5)}});
  ExtrudeFeature e;
  e.sketch = plate.base_sketch;
  e.extent = Extent::UpToPlane;
  e.up_to = FeatureRef{off};
  e.draft = Param::expr("1 deg");
  e.reverse = true;
  t.add(e, "Down");
  RevolveFeature r;
  r.sketch = plate.base_sketch;
  r.axis = PrincipalAxis::X;
  r.angle = Param::of(1.25);
  r.operation = Operation::Cut;
  const Id rid = t.add(r);
  t.set_suppressed(rid, true);
  return t;
}

}  // namespace

TEST_CASE("feature trees round-trip through JSON exactly", "[feature][json]") {
  const FeatureTree tree = everything();
  const std::string text = to_json(tree);
  auto back = feature_tree_from_json(text);
  REQUIRE(back.ok());
  CHECK(*back == tree);
  CHECK(to_json(*back) == text);
}

TEST_CASE("the sample plate file rebuilds", "[feature][json]") {
  auto tree = feature_tree_from_json(read_file(kData / "plate.vfeatures.json"));
  REQUIRE(tree.ok());
  const auto r = rebuild(*tree, kSettings);
  INFO(parts::codes(r.diagnostics));
  REQUIRE(r.ok());
  CHECK(parts::volume(r.body) == Approx(parts::plate_volume(60, 8, 12)).epsilon(1e-9));
}

TEST_CASE("reading rejects malformed files and authored loops", "[feature][json]") {
  const auto code = [](std::string_view text) {
    auto r = feature_tree_from_json(text);
    return r ? std::string("ok") : r.error().code;
  };
  CHECK(code("{") == "feature.json.syntax");
  CHECK(code(R"({"format": "other", "version": 1})") == "feature.json.wrong_format");
  CHECK(code(R"({"format": "vstudio.features", "version": 9})") == "feature.json.version");
  CHECK(code(R"({"format": "vstudio.features", "version": 1})") == "ok");
  CHECK(code(R"({"format": "vstudio.features", "version": 1,
                 "features": [{"id": "00000000000000000000000000000001", "type": "loft"}]})") ==
        "feature.json.unknown_type");
  CHECK(code(R"({"format": "vstudio.features", "version": 1,
                 "features": [{"id": "00000000000000000000000000000001", "type": "extrude"}]})") ==
        "feature.json.missing_key");
  CHECK(code(R"({"format": "vstudio.features", "version": 1,
                 "features": [{"id": "1", "type": "extrude"}]})") == "feature.json.bad_value");
  CHECK(code(R"({"format": "vstudio.features", "version": 1, "variables": [
                   {"id": "00000000000000000000000000000001", "name": "a", "expression": "b"},
                   {"id": "00000000000000000000000000000002", "name": "b", "expression": "a"}]})") ==
        "feature.variable.loop");
  // An unknown variable only fails the rebuild.
  CHECK(code(R"({"format": "vstudio.features", "version": 1, "variables": [
                   {"id": "00000000000000000000000000000001", "name": "a", "expression": "b"}]})") ==
        "ok");
  // Bare numbers and strings are accepted as params and expressions.
  auto t = feature_tree_from_json(R"({"format": "vstudio.features", "version": 1,
      "variables": [{"id": "00000000000000000000000000000001", "name": "a", "expression": 4}],
      "features": [{"id": "00000000000000000000000000000002", "type": "datum_plane",
                    "offset": {"base": {"principal": "zx"}, "distance": "a * 2"}}]})");
  REQUIRE(t.ok());
  const auto r = rebuild(*t, kSettings);
  REQUIRE(r.ok());
  CHECK(r.features[0].plane->origin.y == Approx(8));
}

TEST_CASE("write the sample plate file", "[.generate]") {
  // Run with "[.generate]" to regenerate tests/data/plate.vfeatures.json.
  std::ofstream out(kData / "plate.vfeatures.json", std::ios::binary);
  out << to_json(parts::make_plate(60, 8, 12).tree);
  REQUIRE(out.good());
}

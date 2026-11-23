// Features built from sketches with cubic Béziers (decision 0040): solids,
// sampled side-face names and the references that follow them.

#include <cmath>
#include <numbers>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "parts.hpp"
#include "vs/feature/rebuild.hpp"
#include "vs/feature/serialize.hpp"
#include "vs/kernel/check.hpp"
#include "vs/kernel/queries.hpp"

using namespace vs::feature;
using Catch::Approx;
using parts::kSettings;
using vs::core::dvec2;
using vs::core::dvec3;
using vs::sketch::Sketch;

namespace {

constexpr double kPi = std::numbers::pi;

// A "D" sketch: a line up x = x0 from (x0, y0) to (x0, y0 + 40) and a Bézier
// bulging to +x back down to the start, its handles reaching `bulge` out.
// Its area is 3/5 of the 40 x bulge box the handles span.
struct Dee {
  Sketch sketch;
  Id line, curve, c1, c2;
};
Dee dee(dvec2 at = {0, 0}, double bulge = 40) {
  Dee d;
  const Id bottom = d.sketch.add_point(at);
  const Id top = d.sketch.add_point(at + dvec2{0, 40});
  d.c1 = d.sketch.add_point(at + dvec2{bulge, 40});
  d.c2 = d.sketch.add_point(at + dvec2{bulge, 0});
  d.line = d.sketch.add_line(bottom, top);
  d.curve = d.sketch.add_bezier(top, d.c1, d.c2, bottom);
  return d;
}
constexpr double kDeeArea = 0.6 * 40 * 40;

Id extrude(FeatureTree& tree, const Id& sketch, double depth, Operation op = Operation::Add,
           Extent extent = Extent::Blind) {
  ExtrudeFeature e;
  e.sketch = sketch;
  e.depth = Param::of(depth);
  e.operation = op;
  e.extent = extent;
  return tree.add(e);
}

RebuildResult rebuilt(const FeatureTree& tree) {
  auto r = rebuild(tree, kSettings);
  INFO(parts::codes(r.diagnostics));
  REQUIRE(r.ok());
  CHECK(vs::kernel::check_validity(r.body).empty());
  return r;
}

const NamedFace* named(const FeatureResult& r, std::string_view name) {
  for (const auto& f : r.faces) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

std::string side(const Id& curve) { return "side:" + curve.to_string(); }

// A D extruded 10 mm up from the XY plane.
struct DeeBody {
  FeatureTree tree;
  Dee d;
  Id sketch, body;
};
DeeBody dee_body() {
  DeeBody b;
  b.d = dee();
  b.sketch = b.tree.add(SketchFeature{b.d.sketch, PrincipalPlane::XY});
  b.body = extrude(b.tree, b.sketch, 10);
  return b;
}

}  // namespace

TEST_CASE("An extruded Bezier names its side face by samples", "[feature][bezier]") {
  auto b = dee_body();
  auto r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(kDeeArea * 10).epsilon(1e-9));
  const auto* curve_side = named(*r.find(b.body), side(b.d.curve));
  REQUIRE(curve_side != nullptr);
  CHECK_FALSE(curve_side->cylinder);
  REQUIRE(curve_side->samples.size() == 3);
  CHECK(curve_side->curved());
  // Half way along, the D's bulge faces +x.
  CHECK(curve_side->samples[1].point.x == Approx(30));
  CHECK(curve_side->samples[1].normal.x == Approx(1));
  // The line's side is still a plane.
  const auto* line_side = named(*r.find(b.body), side(b.d.line));
  REQUIRE(line_side != nullptr);
  CHECK(line_side->samples.empty());
  CHECK(line_side->plane.normal.x == Approx(-1));
}

TEST_CASE("A fillet on a Bezier's edge follows the curve as it changes",
          "[feature][bezier][fillet]") {
  auto b = dee_body();
  b.tree.add(FilletFeature{{EdgeRef{FaceRef{b.body, "end"}, FaceRef{b.body, side(b.d.curve)}}},
                           Param::of(1)});
  auto r = rebuilt(b.tree);
  const double rounded = parts::volume(r.body);
  CHECK(rounded < kDeeArea * 10);
  CHECK(rounded > kDeeArea * 10 - 100);

  // Pull the handles in: a slimmer D, and the fillet still finds its edge.
  auto* sk = b.tree.find(b.sketch)->as<SketchFeature>();
  sk->sketch.set_point(b.d.c1, {30, 40});
  sk->sketch.set_point(b.d.c2, {30, 0});
  r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) < 0.6 * 30 * 40 * 10);
  CHECK(parts::volume(r.body) > 0.6 * 30 * 40 * 10 - 100);
}

TEST_CASE("A Bezier pocket's wall faces into the pocket", "[feature][bezier]") {
  FeatureTree t;
  Sketch block;
  const auto rect = parts::add_rectangle(block, {-10, -10}, {60, 60});
  (void)rect;
  const Id block_sketch = t.add(SketchFeature{block, PrincipalPlane::XY});
  const Id base = extrude(t, block_sketch, 20);
  auto d = dee({5, 5}, 30);
  const Id pocket_sketch = t.add(SketchFeature{d.sketch, FaceRef{base, "end"}});
  const Id pocket = extrude(t, pocket_sketch, 0, Operation::Cut, Extent::ThroughAll);
  auto r = rebuilt(t);
  CHECK(parts::volume(r.body) == Approx(70 * 70 * 20 - 0.6 * 40 * 30 * 20).epsilon(1e-9));
  const auto* wall = named(*r.find(pocket), side(d.curve));
  REQUIRE(wall != nullptr);
  REQUIRE(wall->samples.size() == 3);
  // The material lies outside the D, so the wall faces -x at the bulge.
  CHECK(wall->samples[1].normal.x == Approx(-1));

  // A chamfer round the pocket's curved rim, against the block's top.
  t.add(ChamferFeature{{EdgeRef{FaceRef{base, "end"}, FaceRef{pocket, side(d.curve)}}},
                       Param::of(0.5)});
  const double drilled = parts::volume(r.body);
  r = rebuilt(t);
  CHECK(parts::volume(r.body) < drilled);
}

TEST_CASE("A sketch can't sit on a Bezier's side", "[feature][bezier]") {
  auto b = dee_body();
  Sketch s;
  s.add_circle(dvec2{0, 0}, 1);
  const Id on_side = b.tree.add(SketchFeature{s, FaceRef{b.body, side(b.d.curve)}});
  const auto r = rebuild(b.tree, kSettings);
  const auto* f = r.find(on_side);
  REQUIRE(f != nullptr);
  REQUIRE_FALSE(f->ok());
  CHECK(f->diagnostics[0].code == "feature.reference.wrong_kind");
}

TEST_CASE("Pattern copies of a Bezier's side are found again", "[feature][bezier][pattern]") {
  auto b = dee_body();
  LinearPatternFeature lp;
  lp.features = {b.body};
  lp.direction = PrincipalAxis::X;
  lp.count = Param::of(2);
  lp.spacing = Param::of(60);
  const Id pattern = b.tree.add(lp);
  const std::string copy_side = "1:" + b.body.to_string() + ":" + side(b.d.curve);
  const std::string copy_end = "1:" + b.body.to_string() + ":end";
  b.tree.add(FilletFeature{{EdgeRef{FaceRef{pattern, copy_end}, FaceRef{pattern, copy_side}}},
                           Param::of(1)});
  auto r = rebuilt(b.tree);
  const auto* copied = named(*r.find(pattern), copy_side);
  REQUIRE(copied != nullptr);
  REQUIRE(copied->samples.size() == 3);
  CHECK(copied->samples[1].point.x == Approx(90));
  CHECK(parts::volume(r.body) < 2 * kDeeArea * 10);
}

TEST_CASE("Revolves, lofts and sweeps take Beziers", "[feature][bezier]") {
  SECTION("revolve") {
    FeatureTree t;
    const Id s = t.add(SketchFeature{dee().sketch, PrincipalPlane::XY});
    RevolveFeature rv;
    rv.sketch = s;
    rv.axis = PrincipalAxis::Y;
    rv.angle = Param::of(kPi);
    t.add(rv);
    auto r = rebuilt(t);
    CHECK(parts::volume(r.body) > 0);
  }
  SECTION("loft between two Bezier sections") {
    FeatureTree t;
    const Id bottom = t.add(SketchFeature{dee().sketch, PrincipalPlane::XY});
    const Id up = t.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(20)}});
    const Id top = t.add(SketchFeature{dee({0, 0}, 20).sketch, FeatureRef{up}});
    t.add(LoftFeature{{bottom, top}, Operation::Add, false});
    auto r = rebuilt(t);
    CHECK(parts::volume(r.body) > 0.6 * 40 * 20 * 20);
    CHECK(parts::volume(r.body) < 0.6 * 40 * 40 * 20);
  }
  SECTION("sweep along a Bezier path") {
    FeatureTree t;
    Sketch profile;
    profile.add_circle(dvec2{0, 0}, 2);
    const Id p = t.add(SketchFeature{profile, PrincipalPlane::XY});
    // On the ZX plane (sketch x = model Z, y = model X): up from the origin,
    // bending over towards +X.
    Sketch path;
    path.add_bezier(dvec2{0, 0}, dvec2{20, 0}, dvec2{20, 30}, dvec2{40, 30});
    const Id ps = t.add(SketchFeature{path, PrincipalPlane::ZX});
    t.add(SweepFeature{p, ps, Operation::Add, SweepOrientation::FollowPath});
    auto r = rebuilt(t);
    CHECK(parts::volume(r.body) > 0);
  }
}

TEST_CASE("A tree with a Bezier sketch round-trips through JSON", "[feature][bezier]") {
  auto b = dee_body();
  const std::string text = to_json(b.tree);
  auto back = feature_tree_from_json(text);
  REQUIRE(back);
  CHECK(to_json(*back) == text);
}

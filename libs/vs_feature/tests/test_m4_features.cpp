// M4 features on representative parts: sweep, loft, shell, fillet, chamfer
// and linear and circular patterns, with their failure cases. Every failure
// must be a named diagnostic on the feature, leave the body as the features
// above it left it, and let the features below keep rebuilding.

#include <cmath>
#include <numbers>
#include <string>

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
using vs::core::dvec3;
using vs::sketch::Sketch;

namespace {

constexpr double kPi = std::numbers::pi;

Id add_sketch(FeatureTree& tree, const Sketch& s, PlaneRef plane = PrincipalPlane::XY) {
  return tree.add(SketchFeature{s, plane});
}

// A rectangle sketch; returns the sketch feature and fills `rect`.
Id rect_sketch(FeatureTree& tree, dvec2 lo, dvec2 hi, parts::Rect* rect = nullptr,
               PlaneRef plane = PrincipalPlane::XY) {
  Sketch s;
  const auto r = parts::add_rectangle(s, lo, hi);
  if (rect) *rect = r;
  return add_sketch(tree, s, plane);
}

// A circle sketch; returns the sketch feature and fills `circle`.
Id circle_sketch(FeatureTree& tree, dvec2 centre, double radius, Id* circle = nullptr,
                 PlaneRef plane = PrincipalPlane::XY) {
  Sketch s;
  const Id c = s.add_circle(centre, radius);
  if (circle) *circle = c;
  return add_sketch(tree, s, plane);
}

Id extrude(FeatureTree& tree, const Id& sketch, double depth, Operation op = Operation::Add,
           Extent extent = Extent::Blind) {
  ExtrudeFeature e;
  e.sketch = sketch;
  e.depth = Param::of(depth);
  e.operation = op;
  e.extent = extent;
  return tree.add(e);
}

// A w x d x h block from the origin, with its rectangle.
struct Block {
  FeatureTree tree;
  Id sketch, body;
  parts::Rect rect;
  [[nodiscard]] FaceRef face(const std::string& name) const { return {body, name}; }
  [[nodiscard]] FaceRef side(int line) const { return {body, "side:" + rect.l[line].to_string()}; }
};

Block block(double w, double d, double h) {
  Block b;
  b.sketch = rect_sketch(b.tree, {0, 0}, {w, d}, &b.rect);
  b.body = extrude(b.tree, b.sketch, h);
  return b;
}

RebuildResult rebuilt(const FeatureTree& tree) {
  auto r = rebuild(tree, kSettings);
  INFO(parts::codes(r.diagnostics));
  REQUIRE(r.ok());
  CHECK(vs::kernel::check_validity(r.body).empty());
  return r;
}

// Rebuilds a tree whose feature `bad` must fail: checks the failure is a
// named diagnostic on it, that it left the body alone, and that the final
// body is valid. Returns the code of its first diagnostic.
std::string failure_of(const FeatureTree& tree, const Id& bad) {
  const auto r = rebuild(tree, kSettings);
  const auto* f = r.find(bad);
  REQUIRE(f != nullptr);
  if (f->ok()) return "ok";
  REQUIRE_FALSE(f->diagnostics.empty());
  CHECK(f->diagnostics[0].target()->id == bad);
  const auto at = tree.index_of(bad);
  REQUIRE(at.has_value());
  const auto& before = *at == 0 ? vs::kernel::Solid{} : r.features[*at - 1].body;
  CHECK(f->body.is_same(before));
  CHECK(f->faces.empty());
  if (!r.body.is_empty()) CHECK(vs::kernel::check_validity(r.body).empty());
  return f->diagnostics[0].code;
}

const NamedFace* named(const FeatureResult& r, std::string_view name) {
  for (const auto& f : r.faces) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

bool has_code(const vs::core::Diagnostics& ds, std::string_view code) {
  return !ds.with_code(code).empty();
}

// A sweep path sketch on the ZX plane (sketch x = model Z, y = model X):
// up the Z axis for `straight`, then, when `radius` > 0, a quarter turn
// towards +X.
Id path_sketch(FeatureTree& tree, double straight, double radius) {
  Sketch s;
  const Id a = s.add_point({0, 0});
  const Id b = s.add_point({straight, 0});
  s.add_line(a, b);
  if (radius > 0) {
    const Id c = s.add_point({straight, radius});
    const Id e = s.add_point({straight + radius, radius});
    s.add_arc(c, b, e);
  }
  return add_sketch(tree, s, PrincipalPlane::ZX);
}

}  // namespace

// --- sweep --------------------------------------------------------------------

TEST_CASE("sweeps follow straight and bent paths", "[feature][sweep]") {
  FeatureTree t;
  const Id profile = rect_sketch(t, {-2, -2}, {2, 2});
  const Id path = path_sketch(t, 20, 0);
  const Id sw = t.add(SweepFeature{profile, path, Operation::Add, SweepOrientation::FollowPath});
  auto r = rebuilt(t);
  CHECK(parts::volume(r.body) == Approx(16 * 20).epsilon(1e-9));
  const auto& fr = *r.find(sw);
  REQUIRE(named(fr, "start") != nullptr);
  CHECK(named(fr, "start")->plane.normal.z == Approx(-1));
  REQUIRE(named(fr, "end") != nullptr);
  CHECK(named(fr, "end")->plane.origin.z == Approx(20));
  CHECK(named(fr, "end")->plane.normal.z == Approx(1));

  // A quarter turn of radius 10 after 10 mm: Pappus for the bend.
  FeatureTree bent;
  const Id p2 = rect_sketch(bent, {-2, -2}, {2, 2});
  const Id path2 = path_sketch(bent, 10, 10);
  const Id sw2 = bent.add(SweepFeature{p2, path2, Operation::Add, SweepOrientation::FollowPath});
  r = rebuilt(bent);
  CHECK(parts::volume(r.body) == Approx(16 * 10 + 16 * (kPi / 2 * 10)).epsilon(1e-6));
  // The end cap faces +X at the end of the bend.
  const auto* end = named(*r.find(sw2), "end");
  REQUIRE(end != nullptr);
  CHECK(end->plane.normal.x == Approx(1));
  CHECK(end->plane.origin.x == Approx(10));
  CHECK(end->plane.origin.z == Approx(20));

  // A sketch on the end cap follows the path. The cap's frame (frame_for)
  // has x = +Y and y = +Z, so the cap's centre (10, 0, 20) is at (0, 20).
  FeatureTree with_boss = bent;
  const Id boss_sketch = circle_sketch(with_boss, {0, 20}, 1, nullptr, FaceRef{sw2, "end"});
  extrude(with_boss, boss_sketch, 5);
  r = rebuilt(with_boss);
  CHECK(r.body.solid_count() == 1);
  CHECK(vs::kernel::bounding_box(r.body).max.x == Approx(15));

  // With a fixed orientation the profile stays flat while the path turns;
  // it still builds a valid solid.
  bent.find(sw2)->as<SweepFeature>()->orientation = SweepOrientation::Fixed;
  r = rebuilt(bent);
  CHECK(r.find(sw2)->ok());
}

TEST_CASE("sweep cuts remove material along the path", "[feature][sweep]") {
  auto b = block(20, 20, 20);
  const Id profile = circle_sketch(b.tree, {10, 10}, 2);
  // The path runs up through the block's middle: on the ZX plane moved to
  // y = 10 (sketch x = model Z, y = model X), from z = 0 to 20 at x = 10.
  const Id offset = b.tree.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::ZX, Param::of(10)}});
  Sketch path;
  path.add_line(dvec2{0, 10}, dvec2{20, 10});
  const Id path_id = add_sketch(b.tree, path, FeatureRef{offset});
  const Id cut = b.tree.add(SweepFeature{profile, path_id, Operation::Cut, SweepOrientation::FollowPath});
  const auto r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(8000 - kPi * 4 * 20).epsilon(1e-9));
  CHECK(r.find(cut)->operation == Operation::Cut);
}

TEST_CASE("bad sweep paths are named diagnostics", "[feature][sweep]") {
  FeatureTree t;
  const Id profile = rect_sketch(t, {-2, -2}, {2, 2});

  SECTION("no path") {
    Sketch empty;
    empty.add_point({0, 0});
    const Id path = add_sketch(t, empty, PrincipalPlane::ZX);
    const Id sw = t.add(SweepFeature{profile, path, Operation::Add, SweepOrientation::FollowPath});
    CHECK(failure_of(t, sw) == "feature.sweep.no_path");
  }
  SECTION("a branching path") {
    Sketch s;
    const Id a = s.add_point({0, 0});
    s.add_line(a, s.add_point({10, 0}));
    s.add_line(a, s.add_point({0, 10}));
    s.add_line(a, s.add_point({-10, 0}));
    const Id sw = t.add(SweepFeature{profile, add_sketch(t, s, PrincipalPlane::ZX), Operation::Add,
                                     SweepOrientation::FollowPath});
    CHECK(failure_of(t, sw) == "feature.sweep.path_branches");
  }
  SECTION("a path in pieces") {
    Sketch s;
    s.add_line(dvec2{0, 0}, dvec2{10, 0});
    s.add_line(dvec2{12, 0}, dvec2{20, 0});
    const Id sw = t.add(SweepFeature{profile, add_sketch(t, s, PrincipalPlane::ZX), Operation::Add,
                                     SweepOrientation::FollowPath});
    CHECK(failure_of(t, sw) == "feature.sweep.path_disconnected");
  }
  SECTION("a path along the profile's plane") {
    Sketch s;
    s.add_line(dvec2{0, 0}, dvec2{0, 20});  // ZX sketch y = model X: flat in XY
    const Id sw = t.add(SweepFeature{profile, add_sketch(t, s, PrincipalPlane::ZX), Operation::Add,
                                     SweepOrientation::FollowPath});
    CHECK(failure_of(t, sw) == "kernel.sweep.tangent_in_profile");
  }
  SECTION("a profile that isn't a sketch") {
    const Id plane = t.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(1)}});
    const Id sw = t.add(SweepFeature{plane, path_sketch(t, 10, 0), Operation::Add,
                                     SweepOrientation::FollowPath});
    CHECK(failure_of(t, sw) == "feature.reference.wrong_kind");
  }
}

// --- loft -----------------------------------------------------------------------

TEST_CASE("lofts join sections and name their ends", "[feature][loft]") {
  FeatureTree t;
  const Id bottom = rect_sketch(t, {-5, -5}, {5, 5});
  const Id up = t.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::expr("10")}});
  const Id top = rect_sketch(t, {-2, -2}, {2, 2}, nullptr, FeatureRef{up});
  LoftFeature lf;
  lf.sections = {bottom, top};
  lf.ruled = true;
  const Id loft = t.add(lf);
  auto r = rebuilt(t);
  CHECK(parts::volume(r.body) == Approx(10.0 / 3.0 * (100 + 16 + 40)).epsilon(1e-6));
  const auto& fr = *r.find(loft);
  REQUIRE(named(fr, "start") != nullptr);
  CHECK(named(fr, "start")->plane.normal.z == Approx(-1));
  REQUIRE(named(fr, "end") != nullptr);
  CHECK(named(fr, "end")->plane.origin.z == Approx(10));
  CHECK(named(fr, "end")->plane.normal.z == Approx(1));

  // A boss on the loft's end face moves with the datum's offset.
  const Id boss = rect_sketch(t, {-1, -1}, {1, 1}, nullptr, FaceRef{loft, "end"});
  extrude(t, boss, 3);
  r = rebuilt(t);
  CHECK(vs::kernel::bounding_box(r.body).max.z == Approx(13));
  t.find(up)->as<DatumPlaneFeature>()->definition =
      PlaneOffset{PrincipalPlane::XY, Param::expr("15")};
  r = rebuilt(t);
  CHECK(vs::kernel::bounding_box(r.body).max.z == Approx(18));

  // A loft can cut, too.
  FeatureTree c;
  const Id base = rect_sketch(c, {-10, -10}, {10, 10});
  extrude(c, base, 10);
  const Id s0 = rect_sketch(c, {-5, -5}, {5, 5}, nullptr, PrincipalPlane::XY);
  const Id hi = c.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(10)}});
  const Id s1 = rect_sketch(c, {-2, -2}, {2, 2}, nullptr, FeatureRef{hi});
  c.add(LoftFeature{{s0, s1}, Operation::Cut, true});
  r = rebuilt(c);
  CHECK(parts::volume(r.body) == Approx(4000 - 520).epsilon(1e-6));
}

TEST_CASE("bad lofts are named diagnostics", "[feature][loft]") {
  FeatureTree t;
  const Id one = rect_sketch(t, {-5, -5}, {5, 5});
  SECTION("one section") {
    const Id lf = t.add(LoftFeature{{one}, Operation::Add, false});
    CHECK(failure_of(t, lf) == "feature.loft.too_few_sections");
  }
  SECTION("a section with a hole") {
    Sketch s;
    parts::add_rectangle(s, {-5, -5}, {5, 5});
    s.add_circle(dvec2{0, 0}, 1);
    const Id up = t.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(10)}});
    const Id holed = add_sketch(t, s, FeatureRef{up});
    const Id lf = t.add(LoftFeature{{one, holed}, Operation::Add, false});
    CHECK(failure_of(t, lf) == "feature.loft.section_not_single");
  }
  SECTION("a section with two profiles") {
    Sketch s;
    parts::add_rectangle(s, {-5, -5}, {-1, 5});
    parts::add_rectangle(s, {1, -5}, {5, 5});
    const Id up = t.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(10)}});
    const Id two = add_sketch(t, s, FeatureRef{up});
    const Id lf = t.add(LoftFeature{{one, two}, Operation::Add, false});
    CHECK(failure_of(t, lf) == "feature.loft.section_not_single");
  }
}

// --- shell ----------------------------------------------------------------------

TEST_CASE("shells hollow the body, driven by a variable", "[feature][shell]") {
  auto b = block(20, 20, 20);
  REQUIRE(b.tree.variables().add("wall", "2 mm").ok());
  const Id sh = b.tree.add(ShellFeature{{b.face("end")}, Param::expr("wall"), false});
  auto r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(8000 - 16 * 16 * 18).epsilon(1e-9));

  CHECK_FALSE(b.tree.variables().set_expression("wall", "3").has_errors());
  r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(8000 - 14 * 14 * 17).epsilon(1e-9));

  // Earlier faces still resolve after an inward shell: a sketch on a side.
  FeatureTree with_sketch = b.tree;
  const Id s = circle_sketch(with_sketch, {10, 10}, 1, nullptr, b.side(1));
  CHECK(failure_of(with_sketch, s) == "ok");

  // Two open faces, and outwards: the original sides become the inside.
  auto* def = b.tree.find(sh)->as<ShellFeature>();
  def->open.push_back(b.face("start"));
  def->outward = true;
  r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(26 * 26 * 20 - 8000).epsilon(1e-9));
}

TEST_CASE("bad shells are named diagnostics", "[feature][shell]") {
  auto b = block(20, 20, 20);
  SECTION("no faces") {
    const Id sh = b.tree.add(ShellFeature{{}, Param::of(2), false});
    CHECK(failure_of(b.tree, sh) == "feature.shell.no_faces");
  }
  SECTION("zero thickness") {
    const Id sh = b.tree.add(ShellFeature{{b.face("end")}, Param::of(0), false});
    CHECK(failure_of(b.tree, sh) == "feature.shell.bad_thickness");
  }
  SECTION("a face that isn't there") {
    const Id sh = b.tree.add(ShellFeature{{FaceRef{b.body, "side:nonsense"}}, Param::of(2), false});
    CHECK(failure_of(b.tree, sh) == "feature.reference.broken");
  }
  SECTION("walls thicker than a boss's radius") {
    const Id boss_sketch = circle_sketch(b.tree, {10, 10}, 3, nullptr, b.face("end"));
    const Id boss = extrude(b.tree, boss_sketch, 10);
    const Id sh = b.tree.add(ShellFeature{{b.face("start")}, Param::of(4), false});
    CHECK(failure_of(b.tree, sh) == "kernel.shell.too_thick");
    // The features below still rebuild on the unshelled body.
    const Id after = extrude(b.tree, rect_sketch(b.tree, {30, 0}, {40, 10}), 5);
    const auto r = rebuild(b.tree, kSettings);
    CHECK(r.find(after)->ok());
    CHECK(r.find(boss)->ok());
  }
}

// --- fillet and chamfer -------------------------------------------------------

TEST_CASE("fillets round edges found by their faces", "[feature][fillet]") {
  auto b = block(20, 20, 20);
  REQUIRE(b.tree.variables().add("r", "2").ok());
  const Id f = b.tree.add(FilletFeature{{EdgeRef{b.face("end"), b.side(1)}}, Param::expr("r")});
  auto r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(8000 - (4 - kPi) * 20).epsilon(1e-9));

  CHECK_FALSE(b.tree.variables().set_expression("r", "3").has_errors());
  r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(8000 - (9 - 9 * kPi / 4) * 20).epsilon(1e-9));

  // Every edge of the top face, and the same edge twice counts once. The
  // four strips overlap at the corners, so less comes off than four times one.
  const double single = parts::volume(r.body);
  auto* def = b.tree.find(f)->as<FilletFeature>();
  def->edges.push_back(EdgeRef{b.face("end"), std::nullopt});
  r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) < single - 2 * (9 - 9 * kPi / 4) * 20);
  CHECK(parts::volume(r.body) > 8000 - 4 * (9 - 9 * kPi / 4) * 20);

  // The top face is trimmed, not removed: a sketch can still sit on it.
  const Id s = circle_sketch(b.tree, {10, 10}, 2, nullptr, b.face("end"));
  CHECK(failure_of(b.tree, s) == "ok");
}

TEST_CASE("fillets round the edge of a hole", "[feature][fillet]") {
  auto b = block(20, 20, 10);
  Id circle;
  const Id hs = circle_sketch(b.tree, {10, 10}, 3, &circle, b.face("end"));
  const Id hole = extrude(b.tree, hs, 0, Operation::Cut, Extent::ThroughAll);
  auto r = rebuilt(b.tree);
  const double drilled = parts::volume(r.body);
  const auto* wall = named(*r.find(hole), "side:" + circle.to_string());
  REQUIRE(wall != nullptr);
  REQUIRE(wall->cylinder.has_value());
  CHECK(wall->cylinder->radius == Approx(3));
  CHECK(wall->cylinder->inward);

  b.tree.add(FilletFeature{{EdgeRef{b.face("end"), FaceRef{hole, "side:" + circle.to_string()}}},
                           Param::of(1)});
  r = rebuilt(b.tree);
  // A ring comes off the top of the hole. Its section is the corner a 1 mm
  // fillet removes (area 1 - π/4, centroid (10 - 3π) / (12 - 3π) mm from the
  // corner), turned about the axis at radius 3 + that: Pappus.
  const double ring =
      2 * kPi * (3 + (10 - 3 * kPi) / (12 - 3 * kPi)) * (1 - kPi / 4);
  CHECK(parts::volume(r.body) == Approx(drilled - ring).epsilon(1e-6));

  // A curved face can't carry a sketch.
  const Id on_wall = circle_sketch(b.tree, {0, 0}, 1, nullptr, FaceRef{hole, "side:" + circle.to_string()});
  CHECK(failure_of(b.tree, on_wall) == "feature.reference.wrong_kind");
}

TEST_CASE("chamfers bevel edges", "[feature][chamfer]") {
  auto b = block(20, 20, 20);
  const Id c = b.tree.add(ChamferFeature{{EdgeRef{b.face("end"), b.side(0)}}, Param::of(2)});
  auto r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(8000 - 0.5 * 2 * 2 * 20).epsilon(1e-9));
  b.tree.find(c)->as<ChamferFeature>()->distance = Param::expr("3 mm");
  r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(8000 - 0.5 * 3 * 3 * 20).epsilon(1e-9));
}

TEST_CASE("fillets and chamfers that can't be built fall back with a named diagnostic",
          "[feature][fillet][chamfer]") {
  auto b = block(20, 20, 20);
  SECTION("a fillet too big for its faces suggests a radius that works") {
    const Id f =
        b.tree.add(FilletFeature{{EdgeRef{b.face("end"), b.side(1)}}, Param::of(25)});
    const Id after = extrude(b.tree, rect_sketch(b.tree, {30, 0}, {40, 10}), 5);
    CHECK(failure_of(b.tree, f) == "kernel.fillet.failed");
    const auto r = rebuild(b.tree, kSettings);
    CHECK(has_code(r.find(f)->diagnostics, "feature.fallback.works"));
    CHECK(r.find(after)->ok());
  }
  SECTION("a chamfer too big for its faces") {
    const Id c = b.tree.add(ChamferFeature{{EdgeRef{b.face("end"), std::nullopt}}, Param::of(15)});
    CHECK(failure_of(b.tree, c) == "kernel.chamfer.failed");
    const auto r = rebuild(b.tree, kSettings);
    CHECK(has_code(r.find(c)->diagnostics, "feature.fallback.works"));
  }
  SECTION("faces that don't meet") {
    const Id f =
        b.tree.add(FilletFeature{{EdgeRef{b.face("end"), b.face("start")}}, Param::of(1)});
    CHECK(failure_of(b.tree, f) == "feature.reference.broken");
  }
  SECTION("no edges, and a zero radius") {
    const Id none = b.tree.add(FilletFeature{{}, Param::of(1)});
    CHECK(failure_of(b.tree, none) == "feature.fillet.no_edges");
    const Id zero = b.tree.add(ChamferFeature{{EdgeRef{b.face("end"), std::nullopt}}, Param::of(0)});
    CHECK(failure_of(b.tree, zero) == "feature.chamfer.bad_distance");
  }
  SECTION("an edge whose face a later cut removed") {
    const Id cut_sketch = rect_sketch(b.tree, {-1, -1}, {21, 21}, nullptr, b.face("end"));
    ExtrudeFeature e;
    e.sketch = cut_sketch;
    e.operation = Operation::Cut;
    e.depth = Param::of(5);
    b.tree.add(e);
    const Id f = b.tree.add(FilletFeature{{EdgeRef{b.face("end"), b.side(1)}}, Param::of(1)});
    CHECK(failure_of(b.tree, f) == "feature.reference.broken");
  }
}

// --- patterns ---------------------------------------------------------------------

namespace {

// A 100 x 20 x 5 plate with a boss of radius 3 on top at (10, 10), and a
// linear pattern of the boss along X, `n` copies 20 mm apart.
struct Bosses {
  FeatureTree tree;
  Id n, plate, boss, pattern;
};

Bosses bosses(int n) {
  Bosses b;
  b.n = b.tree.variables().add("n", std::to_string(n)).value();
  const Id base = rect_sketch(b.tree, {0, 0}, {100, 20});
  b.plate = extrude(b.tree, base, 5);
  const Id bs = circle_sketch(b.tree, {10, 10}, 3, nullptr, FaceRef{b.plate, "end"});
  b.boss = extrude(b.tree, bs, 5);
  LinearPatternFeature lp;
  lp.features = {b.boss};
  lp.direction = PrincipalAxis::X;
  lp.count = Param::expr("n");
  lp.spacing = Param::of(20);
  b.pattern = b.tree.add(lp);
  return b;
}

double bosses_volume(int n) { return 100 * 20 * 5 + n * kPi * 9 * 5; }

}  // namespace

TEST_CASE("linear patterns rebuild when the count variable changes", "[feature][pattern]") {
  auto b = bosses(3);
  auto r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(bosses_volume(3)).epsilon(1e-9));

  for (int n : {5, 1, 4}) {
    INFO("n = " << n);
    CHECK_FALSE(b.tree.variables().set_expression(b.n, std::to_string(n)).has_errors());
    r = rebuilt(b.tree);
    CHECK(parts::volume(r.body) == Approx(bosses_volume(n)).epsilon(1e-9));
    CHECK(vs::kernel::bounding_box(r.body).max.z == Approx(10));
  }

  // The copies' faces are named: put a pin on top of the second copy.
  const std::string top = "2:" + b.boss.to_string() + ":end";
  const auto* face = named(*r.find(b.pattern), top);
  REQUIRE(face != nullptr);
  // The seed's "end" is at its sketch plane's origin (0, 0, 10), moved 40.
  CHECK(face->plane.origin.x == Approx(40));
  CHECK(face->plane.origin.z == Approx(10));
  const Id pin = circle_sketch(b.tree, {50, 10}, 1, nullptr, FaceRef{b.pattern, top});
  extrude(b.tree, pin, 2);
  r = rebuilt(b.tree);
  CHECK(vs::kernel::bounding_box(r.body).max.z == Approx(12));

  // Fewer copies and the face is gone: a broken reference on the sketch.
  CHECK_FALSE(b.tree.variables().set_expression(b.n, "2").has_errors());
  CHECK(failure_of(b.tree, pin) == "feature.reference.broken");
}

TEST_CASE("circular patterns spread copies around an axis", "[feature][pattern]") {
  FeatureTree t;
  const Id base = rect_sketch(t, {-20, -20}, {20, 20});
  const Id plate = extrude(t, base, 5);
  const Id hs = circle_sketch(t, {12, 0}, 2, nullptr, FaceRef{plate, "end"});
  const Id hole = extrude(t, hs, 0, Operation::Cut, Extent::ThroughAll);
  CircularPatternFeature cp;
  cp.features = {hole};
  cp.axis = PrincipalAxis::Z;
  cp.count = Param::of(6);
  const Id pattern = t.add(cp);
  auto r = rebuilt(t);
  CHECK(parts::volume(r.body) == Approx(8000 - 6 * kPi * 4 * 5).epsilon(1e-9));

  // Three copies over a quarter turn: holes at 0°, 45° and 90°.
  auto* def = t.find(pattern)->as<CircularPatternFeature>();
  def->count = Param::of(3);
  def->angle = Param::expr("90 deg");
  r = rebuilt(t);
  CHECK(parts::volume(r.body) == Approx(8000 - 3 * kPi * 4 * 5).epsilon(1e-9));
  using vs::kernel::PointClass;
  const double s = 12 * std::sqrt(0.5);
  CHECK(vs::kernel::classify(r.body, {0, 12, 2.5}, kSettings) == PointClass::Outside);
  CHECK(vs::kernel::classify(r.body, {s, s, 2.5}, kSettings) == PointClass::Outside);
  CHECK(vs::kernel::classify(r.body, {-12, 0, 2.5}, kSettings) == PointClass::Inside);
  // Reversed, they turn the other way.
  def->reverse = true;
  r = rebuilt(t);
  CHECK(vs::kernel::classify(r.body, {0, -12, 2.5}, kSettings) == PointClass::Outside);
  CHECK(vs::kernel::classify(r.body, {0, 12, 2.5}, kSettings) == PointClass::Inside);
}

TEST_CASE("patterns copy sweeps and lofts, and several features at once", "[feature][pattern]") {
  FeatureTree t;
  const Id base = rect_sketch(t, {-30, -10}, {30, 10});
  const Id plate = extrude(t, base, 2);
  // A loft boss and a swept post on the plate.
  const Id s0 = rect_sketch(t, {-25, -2}, {-21, 2}, nullptr, FaceRef{plate, "end"});
  const Id up = t.add(DatumPlaneFeature{PlaneOffset{FaceRef{plate, "end"}, Param::of(4)}});
  const Id s1 = rect_sketch(t, {-24, -1}, {-22, 1}, nullptr, FeatureRef{up});
  const Id loft = t.add(LoftFeature{{s0, s1}, Operation::Add, true});
  const Id profile = circle_sketch(t, {-15, 0}, 1, nullptr, FaceRef{plate, "end"});
  Sketch path;
  path.add_line(dvec2{2, -15}, dvec2{8, -15});  // ZX: model z 2..8 at x = -15
  const Id path_id = add_sketch(t, path, PrincipalPlane::ZX);
  const Id post = t.add(SweepFeature{profile, path_id, Operation::Add, SweepOrientation::FollowPath});
  auto r = rebuilt(t);
  const double one = parts::volume(r.body) - 60 * 20 * 2;

  LinearPatternFeature lp;
  lp.features = {loft, post};
  lp.direction = PrincipalAxis::X;
  lp.count = Param::of(3);
  lp.spacing = Param::of(18);
  t.add(lp);
  r = rebuilt(t);
  CHECK(parts::volume(r.body) - 60 * 20 * 2 == Approx(3 * one).epsilon(1e-6));
}

TEST_CASE("a shelled, chamfered tray with a row of drain holes follows its variables",
          "[feature][pattern][shell][chamfer]") {
  auto tray = parts::make_tray(2, 4);
  auto r = rebuilt(tray.tree);
  CHECK(parts::volume(r.body) == Approx(parts::tray_volume(2, 4)).epsilon(1e-9));
  CHECK_FALSE(tray.tree.variables().set_expression(tray.holes, "5").has_errors());
  CHECK_FALSE(tray.tree.variables().set_expression(tray.wall, "3 mm").has_errors());
  r = rebuilt(tray.tree);
  CHECK(parts::volume(r.body) == Approx(parts::tray_volume(3, 5)).epsilon(1e-9));
}

TEST_CASE("bad patterns are named diagnostics", "[feature][pattern]") {
  auto b = bosses(3);
  auto* lp = b.tree.find(b.pattern)->as<LinearPatternFeature>();
  SECTION("a count that isn't a whole number") {
    lp->count = Param::of(2.5);
    CHECK(failure_of(b.tree, b.pattern) == "feature.pattern.bad_count");
  }
  SECTION("a count of zero") {
    CHECK_FALSE(b.tree.variables().set_expression(b.n, "0").has_errors());
    CHECK(failure_of(b.tree, b.pattern) == "feature.pattern.bad_count");
  }
  SECTION("too many copies") {
    lp->count = Param::of(5000);
    CHECK(failure_of(b.tree, b.pattern) == "feature.pattern.too_many");
  }
  SECTION("zero spacing") {
    lp->spacing = Param::of(0);
    CHECK(failure_of(b.tree, b.pattern) == "feature.pattern.bad_spacing");
  }
  SECTION("nothing to copy") {
    lp->features.clear();
    CHECK(failure_of(b.tree, b.pattern) == "feature.pattern.no_features");
  }
  SECTION("copying something that isn't a solid feature") {
    lp->features = {b.tree.features()[0].id};  // the base sketch
    CHECK(failure_of(b.tree, b.pattern) == "feature.pattern.not_solid_feature");
  }
  SECTION("copying a feature that failed") {
    b.tree.find(b.boss)->as<ExtrudeFeature>()->depth = Param::of(0);
    CHECK(failure_of(b.tree, b.pattern) == "feature.reference.upstream_failed");
  }
  SECTION("a bad circular angle") {
    CircularPatternFeature cp;
    cp.features = {b.boss};
    cp.angle = Param::of(0);
    const Id c = b.tree.add(cp);
    CHECK(failure_of(b.tree, c) == "feature.pattern.bad_angle");
  }
}

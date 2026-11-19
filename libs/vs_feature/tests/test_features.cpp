#include <cmath>
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
using vs::sketch::Sketch;

namespace {

constexpr double kPi = std::numbers::pi;

// A tree with one sketch holding a w x h rectangle at the origin.
struct Block {
  FeatureTree tree;
  Id sketch;
  parts::Rect rect;
};

Block block_sketch(double w, double h, PlaneRef plane = PrincipalPlane::XY) {
  Block b;
  Sketch s;
  b.rect = parts::add_rectangle(s, {0, 0}, {w, h});
  b.sketch = b.tree.add(SketchFeature{s, plane});
  return b;
}

Id extrude(FeatureTree& tree, const Id& sketch, Operation op, Extent extent, double depth = 10,
           bool reverse = false) {
  ExtrudeFeature e;
  e.sketch = sketch;
  e.operation = op;
  e.extent = extent;
  e.depth = Param::of(depth);
  e.reverse = reverse;
  return tree.add(e);
}

RebuildResult rebuilt(const FeatureTree& tree) {
  auto r = rebuild(tree, kSettings);
  INFO(parts::codes(r.diagnostics));
  REQUIRE(r.ok());
  CHECK(vs::kernel::check_validity(r.body).empty());
  return r;
}

std::string error_of(const FeatureTree& tree, const Id& feature) {
  const auto r = rebuild(tree, kSettings);
  const auto* f = r.find(feature);
  REQUIRE(f != nullptr);
  if (f->diagnostics.empty() || !f->diagnostics.has_errors()) return "ok";
  CHECK(f->diagnostics[0].target()->id == feature);
  return f->diagnostics[0].code;
}

const NamedFace* named(const FeatureResult& r, std::string_view name) {
  for (const auto& f : r.faces) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("blind and reversed extrusions", "[feature][extrude]") {
  auto b = block_sketch(20, 30);
  const Id e = extrude(b.tree, b.sketch, Operation::Add, Extent::Blind, 10);
  auto r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(6000).epsilon(1e-12));
  auto box = vs::kernel::bounding_box(r.body);
  CHECK(box.min.z == Approx(0).margin(1e-9));
  CHECK(box.max.z == Approx(10));

  // Named faces: start, end and one side per line.
  const auto& fr = *r.find(e);
  REQUIRE(named(fr, "start") != nullptr);
  CHECK(named(fr, "start")->plane.normal.z == Approx(-1));
  CHECK(named(fr, "end")->plane.origin.z == Approx(10));
  for (const Id& line : b.rect.l) {
    INFO(line.to_string());
    CHECK(named(fr, "side:" + line.to_string()) != nullptr);
  }
  // The bottom line's side faces -Y.
  CHECK(named(fr, "side:" + b.rect.l[0].to_string())->plane.normal.y == Approx(-1));

  b.tree.find(e)->as<ExtrudeFeature>()->reverse = true;
  r = rebuilt(b.tree);
  box = vs::kernel::bounding_box(r.body);
  CHECK(box.min.z == Approx(-10));
  CHECK(box.max.z == Approx(0).margin(1e-9));
}

TEST_CASE("symmetric extrusions straddle the sketch plane", "[feature][extrude]") {
  auto b = block_sketch(20, 30);
  extrude(b.tree, b.sketch, Operation::Add, Extent::Symmetric, 12);
  const auto r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(7200).epsilon(1e-12));
  const auto box = vs::kernel::bounding_box(r.body);
  CHECK(box.min.z == Approx(-6));
  CHECK(box.max.z == Approx(6));
}

TEST_CASE("drafted extrusions taper", "[feature][extrude][draft]") {
  auto b = block_sketch(20, 20);
  const Id e = extrude(b.tree, b.sketch, Operation::Add, Extent::Blind, 10);
  b.tree.find(e)->as<ExtrudeFeature>()->draft = Param::expr("5 deg");
  const auto r = rebuilt(b.tree);
  const double top = 20 - 2 * 10 * std::tan(5 * kPi / 180);
  CHECK(parts::volume(r.body) ==
        Approx(10.0 / 3.0 * (400 + top * top + 20 * top)).epsilon(1e-9));
  // The side faces lean in: the right side's normal tips up.
  const auto* right = named(*r.find(e), "side:" + b.rect.l[1].to_string());
  REQUIRE(right != nullptr);
  CHECK(right->plane.normal.x == Approx(std::cos(5 * kPi / 180)));
  CHECK(right->plane.normal.z == Approx(std::sin(5 * kPi / 180)));

  // Symmetric with draft tapers both ways from the sketch plane.
  b.tree.find(e)->as<ExtrudeFeature>()->extent = Extent::Symmetric;
  const auto s = rebuilt(b.tree);
  const double half_top = 20 - 2 * 5 * std::tan(5 * kPi / 180);
  CHECK(parts::volume(s.body) ==
        Approx(2 * 5.0 / 3.0 * (400 + half_top * half_top + 20 * half_top)).epsilon(1e-9));
}

TEST_CASE("cuts: blind, through all and up to a plane", "[feature][extrude][cut]") {
  auto b = block_sketch(40, 40);
  const Id base = extrude(b.tree, b.sketch, Operation::Add, Extent::Blind, 10);
  // A 10 x 10 pocket from the top face, 4 deep.
  Sketch pocket;
  parts::add_rectangle(pocket, {5, 5}, {15, 15});
  const Id pocket_sketch = b.tree.add(SketchFeature{pocket, FaceRef{base, "end"}});
  const Id pocket_cut = extrude(b.tree, pocket_sketch, Operation::Cut, Extent::Blind, 4);
  auto r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(16000 - 400).epsilon(1e-12));
  // The pocket's floor faces up, at z = 6.
  const auto* floor = named(*r.find(pocket_cut), "end");
  REQUIRE(floor != nullptr);
  CHECK(floor->plane.origin.z == Approx(6));
  CHECK(floor->plane.normal.z == Approx(1));

  // Through all: a 5 x 5 square from the top face goes through.
  Sketch through;
  parts::add_rectangle(through, {25, 25}, {30, 30});
  const Id through_sketch = b.tree.add(SketchFeature{through, FaceRef{base, "end"}});
  extrude(b.tree, through_sketch, Operation::Cut, Extent::ThroughAll);
  r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(16000 - 400 - 250).epsilon(1e-12));

  // Up to a plane: from the bottom up to z = 7 (a datum), 3 x 3.
  const Id seven = b.tree.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(7)}});
  Sketch up;
  parts::add_rectangle(up, {32, 2}, {35, 5});
  const Id up_sketch = b.tree.add(SketchFeature{up, PrincipalPlane::XY});
  ExtrudeFeature to_plane;
  to_plane.sketch = up_sketch;
  to_plane.operation = Operation::Cut;
  to_plane.extent = Extent::UpToPlane;
  to_plane.up_to = FeatureRef{seven};
  to_plane.reverse = true;  // a cut goes against the normal; this one goes up
  b.tree.add(to_plane);
  r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(16000 - 400 - 250 - 63).epsilon(1e-12));
}

TEST_CASE("extrusion errors name the feature", "[feature][extrude]") {
  auto b = block_sketch(10, 10);
  const Id bad_depth = extrude(b.tree, b.sketch, Operation::Add, Extent::Blind, 0);
  CHECK(error_of(b.tree, bad_depth) == "feature.extrude.bad_depth");
  b.tree.remove(bad_depth);

  const Id cut_nothing = extrude(b.tree, b.sketch, Operation::Cut, Extent::Blind, 5);
  CHECK(error_of(b.tree, cut_nothing) == "feature.cut.no_body");
  b.tree.remove(cut_nothing);

  const Id through_nothing = extrude(b.tree, b.sketch, Operation::Add, Extent::ThroughAll);
  CHECK(error_of(b.tree, through_nothing) == "feature.extrude.through_all_needs_body");
  b.tree.remove(through_nothing);

  ExtrudeFeature slanted;
  slanted.sketch = b.sketch;
  slanted.extent = Extent::UpToPlane;
  slanted.up_to = PrincipalPlane::YZ;
  const Id slanted_id = b.tree.add(slanted);
  CHECK(error_of(b.tree, slanted_id) == "feature.extrude.up_to_not_parallel");
  b.tree.find(slanted_id)->as<ExtrudeFeature>()->up_to = PrincipalPlane::XY;
  CHECK(error_of(b.tree, slanted_id) == "feature.extrude.up_to_behind");
  b.tree.remove(slanted_id);

  // Extruding something that isn't a sketch, or a sketch with nothing closed.
  const Id plane = b.tree.add(DatumPlaneFeature{PlaneOffset{}});
  ExtrudeFeature of_plane;
  of_plane.sketch = plane;
  const Id of_plane_id = b.tree.add(of_plane);
  CHECK(error_of(b.tree, of_plane_id) == "feature.reference.wrong_kind");
  Sketch open;
  open.add_line(dvec2{0, 0}, dvec2{5, 5});
  const Id open_id = b.tree.add(SketchFeature{open});
  const Id of_open = extrude(b.tree, open_id, Operation::Add, Extent::Blind);
  CHECK(error_of(b.tree, of_open) == "sketch.profile.open");
  Sketch empty;
  const Id empty_id = b.tree.add(SketchFeature{empty});
  const Id of_empty = extrude(b.tree, empty_id, Operation::Add, Extent::Blind);
  CHECK(error_of(b.tree, of_empty) == "feature.sketch.no_profile");
}

TEST_CASE("profiles with holes and several profiles", "[feature][extrude]") {
  FeatureTree tree;
  Sketch s;
  parts::add_rectangle(s, {0, 0}, {30, 30});
  s.add_circle(dvec2{15, 15}, 5);        // a hole in the square
  parts::add_rectangle(s, {40, 0}, {50, 10});  // a second, separate profile
  const Id sk = tree.add(SketchFeature{s});
  extrude(tree, sk, Operation::Add, Extent::Blind, 2);
  const auto r = rebuilt(tree);
  CHECK(r.body.solid_count() == 2);
  CHECK(parts::volume(r.body) == Approx((900 - kPi * 25 + 100) * 2).epsilon(1e-9));
}

TEST_CASE("revolves: full, partial and cut", "[feature][revolve]") {
  // A 10 x 20 rectangle beside the Y axis in the XY plane.
  auto b = block_sketch(10, 20);
  RevolveFeature full;
  full.sketch = b.sketch;
  full.axis = PrincipalAxis::Y;
  const Id rev = b.tree.add(full);
  auto r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(kPi * 100 * 20).epsilon(1e-9));
  CHECK(r.find(rev)->faces.empty());

  // A quarter turn, driven by a variable.
  REQUIRE(b.tree.variables().add("sweep", "90 deg").ok());
  b.tree.find(rev)->as<RevolveFeature>()->angle = Param::expr("sweep");
  r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) == Approx(kPi * 100 * 20 / 4).epsilon(1e-9));
  const auto& fr = *r.find(rev);
  // Turning +X about +Y by +90° heads to -Z, so the quarter lies in x >= 0,
  // z <= 0: the start face (on the sketch plane z = 0) faces +Z and the end
  // face (on x = 0) faces -X.
  REQUIRE(named(fr, "start") != nullptr);
  REQUIRE(named(fr, "end") != nullptr);
  CHECK(named(fr, "start")->plane.normal.z == Approx(1));
  CHECK(named(fr, "end")->plane.normal.x == Approx(-1));
  const auto box = vs::kernel::bounding_box(r.body);
  CHECK(box.min.z == Approx(-10));
  CHECK(box.max.z == Approx(0).margin(1e-9));

  // A sketch on the start face sits on the plane z = 0 facing +Z.
  Sketch on_start;
  parts::add_rectangle(on_start, {1, 1}, {2, 2});
  const Id on_start_id = b.tree.add(SketchFeature{on_start, FaceRef{rev, "start"}});
  r = rebuilt(b.tree);
  CHECK(r.find(on_start_id)->plane->normal().z == Approx(1));
  b.tree.remove(on_start_id);

  // Back to a full turn, then a revolved cut: a 2 x 2 ring groove in the
  // cylinder's outside, about the same axis.
  b.tree.find(rev)->as<RevolveFeature>()->angle = Param::of(2 * kPi);
  Sketch groove;
  parts::add_rectangle(groove, {8, 9}, {10, 11});
  const Id groove_id = b.tree.add(SketchFeature{groove});
  RevolveFeature cut;
  cut.sketch = groove_id;
  cut.operation = Operation::Cut;
  cut.axis = PrincipalAxis::Y;
  b.tree.add(cut);
  r = rebuilt(b.tree);
  CHECK(parts::volume(r.body) ==
        Approx(kPi * 100 * 20 - kPi * (100 - 64) * 2).epsilon(1e-9));
}

TEST_CASE("revolve errors", "[feature][revolve]") {
  auto b = block_sketch(10, 20);
  RevolveFeature rv;
  rv.sketch = b.sketch;
  rv.axis = PrincipalAxis::Z;  // perpendicular to the sketch plane
  const Id id = b.tree.add(rv);
  CHECK(error_of(b.tree, id) == "feature.revolve.axis_not_in_plane");
  auto* f = b.tree.find(id)->as<RevolveFeature>();
  f->axis = PrincipalAxis::Y;
  f->angle = Param::of(0);
  CHECK(error_of(b.tree, id) == "feature.revolve.bad_angle");
  f->angle = Param::expr("400 deg");
  CHECK(error_of(b.tree, id) == "feature.revolve.bad_angle");
  f->angle = Param::of(1);
  // An axis through the middle of the profile.
  f->axis = SketchEntityRef{b.sketch, b.rect.l[0]};  // along the bottom edge: fine
  CHECK(error_of(b.tree, id) == "ok");
  auto* sk = b.tree.sketch(b.sketch);
  const Id middle = sk->add_line(dvec2{5, -5}, dvec2{5, 25}, /*construction=*/true);
  f->axis = SketchEntityRef{b.sketch, middle};
  CHECK(error_of(b.tree, id) == "kernel.revolve.axis_crosses_profile");
  f->axis = SketchEntityRef{b.sketch, sk->entities().front().id};  // a point, not a line
  CHECK(error_of(b.tree, id) == "feature.reference.wrong_kind");
}

TEST_CASE("booleans treat faces within the tolerance as touching", "[feature][fuzzy]") {
  // Decision 0002: a boss sketched on a plane half a tolerance above the
  // block's top face still joins the block.
  auto b = block_sketch(20, 20);
  extrude(b.tree, b.sketch, Operation::Add, Extent::Blind, 10);
  const Id nearly =
      b.tree.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(10 + 0.5e-6)}});
  Sketch boss;
  parts::add_rectangle(boss, {5, 5}, {15, 15});
  const Id boss_sketch = b.tree.add(SketchFeature{boss, FeatureRef{nearly}});
  extrude(b.tree, boss_sketch, Operation::Add, Extent::Blind, 5);
  const auto r = rebuilt(b.tree);
  CHECK(r.body.solid_count() == 1);
  CHECK(parts::volume(r.body) == Approx(4000 + 500).epsilon(1e-6));
}

TEST_CASE("every body-changing feature gets a tolerance audit", "[feature][tolerance]") {
  auto plate = parts::make_plate();
  const auto r = rebuilt(plate.tree);
  for (const Id& id : {plate.base, plate.hole}) {
    const auto* f = r.find(id);
    CHECK(f->max_tolerance > 0);
    CHECK(f->max_tolerance <= kSettings.linear_tolerance_mm);
  }
  CHECK(r.find(plate.base_sketch)->max_tolerance == 0);
  CHECK(r.diagnostics.with_code("kernel.tolerance.exceeded").empty());
}

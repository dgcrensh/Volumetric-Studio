#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "canonical.hpp"
#include "vs/kernel/check.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/sketch/profile.hpp"
#include "vs/sketch/solver.hpp"

using namespace vs::sketch;
using vs::core::ProductSettings;

namespace {

const ProductSettings kSettings = ProductSettings::defaults();
constexpr double kPi = std::numbers::pi;

// Extrudes `face` 10 mm along +z and classifies a point at mid-height.
vs::kernel::PointClass where(const vs::kernel::Face& face, dvec2 p) {
  auto solid = vs::kernel::make_prism(face, {0, 0, 10}, kSettings);
  REQUIRE(solid);
  return vs::kernel::classify(*solid, {p.x, p.y, 5}, kSettings);
}

double polygon_area(const std::vector<dvec2>& pts) {
  double a = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    a += vs::core::cross(pts[i], pts[(i + 1) % pts.size()]);
  }
  return 0.5 * a;
}

}  // namespace

TEST_CASE("A rectangle is one profile and a valid face", "[sketch][profile]") {
  auto r = test::rectangle(40, 25);
  REQUIRE(solve(r.sketch, kSettings).ok());
  auto profiles = extract_profiles(r.sketch, kSettings);
  REQUIRE(profiles);
  REQUIRE(profiles->size() == 1);
  CHECK(profiles->front().holes.empty());
  CHECK(profiles->front().outer.edges.size() == 4);
  CHECK(profiles->front().area() == Catch::Approx(1000));

  auto face = make_face(r.sketch, profiles->front(), kSettings);
  REQUIRE(face);
  CHECK(vs::kernel::check_validity(*face).empty());
  CHECK(vs::kernel::surface_area(*face) == Catch::Approx(1000).epsilon(1e-9));
}

TEST_CASE("A slot's face has exact arcs", "[sketch][profile]") {
  auto r = test::slot(50, 8);
  REQUIRE(solve(r.sketch, kSettings).ok());
  auto faces = make_faces(r.sketch, kSettings);
  REQUIRE(faces);
  REQUIRE(faces->size() == 1);
  const double expected = 50 * 16 + kPi * 64;
  CHECK(vs::kernel::surface_area(faces->front()) == Catch::Approx(expected).epsilon(1e-9));
  auto profiles = extract_profiles(r.sketch, kSettings);
  CHECK(profiles->front().area() == Catch::Approx(expected).epsilon(1e-9));
}

TEST_CASE("A gear-like profile becomes one face", "[sketch][profile]") {
  auto r = test::gear(12, 40, 46);
  REQUIRE(solve(r.sketch, kSettings).ok());
  auto faces = make_faces(r.sketch, kSettings);
  REQUIRE(faces);
  REQUIRE(faces->size() == 1);  // the construction spokes and circles take no part
  CHECK(vs::kernel::check_validity(faces->front()).empty());
  CHECK(vs::kernel::surface_area(faces->front()) ==
        Catch::Approx(polygon_area(r.expected)).epsilon(1e-9));
}

TEST_CASE("A bolt circle plate has its holes in the right places", "[sketch][profile]") {
  auto r = test::bolt_circle(50, 35, 6, 8);
  REQUIRE(solve(r.sketch, kSettings).ok());
  auto profiles = extract_profiles(r.sketch, kSettings);
  REQUIRE(profiles);
  REQUIRE(profiles->size() == 1);
  CHECK(profiles->front().holes.size() == 6);

  auto face = make_face(r.sketch, profiles->front(), kSettings);
  REQUIRE(face);
  CHECK(vs::kernel::check_validity(*face).empty());
  CHECK(vs::kernel::surface_area(*face) ==
        Catch::Approx(kPi * (50 * 50 - 6 * 4 * 4)).epsilon(1e-9));
  // Material between the holes, none at their centres.
  for (int k = 0; k < 6; ++k) {
    const double a = 2 * kPi * k / 6;
    INFO("hole " << k);
    CHECK(where(*face, {35 * std::cos(a), 35 * std::sin(a)}) == vs::kernel::PointClass::Outside);
    CHECK(where(*face, {35 * std::cos(a + kPi / 6), 35 * std::sin(a + kPi / 6)}) ==
          vs::kernel::PointClass::Inside);
  }
  CHECK(where(*face, {0, 0}) == vs::kernel::PointClass::Inside);
}

TEST_CASE("Loops nest into outers, holes and islands", "[sketch][profile]") {
  Sketch s;
  // A 100 x 60 plate with a rectangular window, a slot-free round hole, and
  // an island inside the window.
  auto rect = [&](dvec2 lo, dvec2 hi) {
    const Id a = s.add_point(lo);
    const Id b = s.add_point({hi.x, lo.y});
    const Id c = s.add_point(hi);
    const Id d = s.add_point({lo.x, hi.y});
    // Deliberately wound clockwise; extraction orients loops itself.
    s.add_line(a, d);
    s.add_line(d, c);
    s.add_line(c, b);
    s.add_line(b, a);
  };
  rect({0, 0}, {100, 60});
  rect({10, 10}, {50, 50});           // window
  s.add_circle(dvec2{30, 30}, 5);     // island in the window
  s.add_circle(dvec2{75, 30}, 10);    // round hole
  s.add_circle(dvec2{75, 30}, 15, true);  // construction: ignored

  auto profiles = extract_profiles(s, kSettings);
  REQUIRE(profiles);
  REQUIRE(profiles->size() == 2);
  const Profile& plate = (*profiles)[0];
  const Profile& island = (*profiles)[1];
  CHECK(plate.holes.size() == 2);
  CHECK(island.holes.empty());
  CHECK(plate.area() == Catch::Approx(6000 - 1600 - kPi * 100));
  CHECK(island.area() == Catch::Approx(kPi * 25));

  auto faces = make_faces(s, kSettings);
  REQUIRE(faces);
  REQUIRE(faces->size() == 2);
  CHECK(where((*faces)[0], {5, 5}) == vs::kernel::PointClass::Inside);
  CHECK(where((*faces)[0], {20, 20}) == vs::kernel::PointClass::Outside);  // window
  CHECK(where((*faces)[0], {75, 30}) == vs::kernel::PointClass::Outside);  // hole
  CHECK(where((*faces)[1], {30, 30}) == vs::kernel::PointClass::Inside);   // island
}

TEST_CASE("Curve ends join by position as well as by shared points", "[sketch][profile]") {
  Sketch s;
  // A triangle whose lines each have their own end points.
  s.add_line(dvec2{0, 0}, dvec2{10, 0});
  s.add_line(dvec2{10, 0}, dvec2{0, 10});
  s.add_line(dvec2{0, 10}, dvec2{0, 0});
  auto profiles = extract_profiles(s, kSettings);
  REQUIRE(profiles);
  REQUIRE(profiles->size() == 1);
  CHECK(profiles->front().area() == Catch::Approx(50));
}

TEST_CASE("Profiles on a placed sketch land on its plane", "[sketch][profile]") {
  auto r = test::rectangle(40, 25);
  r.sketch.set_plane({{0, 0, 7}, {0, 1, 0}, {0, 0, 1}});  // the y-z plane at x = 0, shifted
  REQUIRE(solve(r.sketch, kSettings).ok());
  auto faces = make_faces(r.sketch, kSettings);
  REQUIRE(faces);
  const auto box = vs::kernel::bounding_box(faces->front());
  CHECK(box.min.x == Catch::Approx(0).margin(1e-6));
  CHECK(box.max.x == Catch::Approx(0).margin(1e-6));
  CHECK(box.min.y == Catch::Approx(0).margin(1e-6));
  CHECK(box.max.y == Catch::Approx(40).margin(1e-6));
  CHECK(box.min.z == Catch::Approx(7).margin(1e-6));
  CHECK(box.max.z == Catch::Approx(32).margin(1e-6));
}

TEST_CASE("Open, branching and crossing geometry is reported", "[sketch][profile]") {
  Sketch s;
  const Id a = s.add_line(dvec2{0, 0}, dvec2{10, 0});
  const Id b = s.add_line(dvec2{10, 0}, dvec2{10, 10});

  SECTION("an open chain") {
    auto result = extract_profiles(s, kSettings);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "sketch.profile.open");
    const Id target = result.error().target()->id;
    CHECK((target == a || target == b));
  }
  SECTION("three ends at one point") {
    s.add_line(dvec2{10, 10}, dvec2{0, 0});
    s.add_line(dvec2{10, 0}, dvec2{20, 0});
    s.add_line(dvec2{20, 0}, dvec2{10, 10});
    auto result = extract_profiles(s, kSettings);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "sketch.profile.branching");
    CHECK(result.error().subjects.size() == 4);  // three curves and the sketch
  }
  SECTION("two loops that cross") {
    s.add_line(dvec2{10, 10}, dvec2{0, 0});
    s.add_circle(dvec2{10, 0}, 3);
    auto result = extract_profiles(s, kSettings);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "sketch.profile.loops_cross");
  }
  SECTION("construction geometry closes nothing") {
    s.add_line(dvec2{10, 10}, dvec2{0, 0}, /*construction=*/true);
    auto result = extract_profiles(s, kSettings);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "sketch.profile.open");
  }
}

TEST_CASE("A sketch with no closed geometry has no profiles", "[sketch][profile]") {
  Sketch s;
  s.add_point({1, 1});
  auto profiles = extract_profiles(s, kSettings);
  REQUIRE(profiles);
  CHECK(profiles->empty());
}

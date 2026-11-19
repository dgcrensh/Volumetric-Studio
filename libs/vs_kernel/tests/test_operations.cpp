#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/kernel.hpp"

using namespace vs::kernel;
using Catch::Approx;
using vs::core::dbox3;
using vs::core::dtransform;
using vs::core::dvec3;
using vs::core::ProductSettings;

namespace {
const ProductSettings kSettings = ProductSettings::defaults();
constexpr double kPi = std::numbers::pi;
constexpr double kRel = 1e-9;

Solid box(dvec3 lo, dvec3 hi) { return make_box(dbox3::from_min_max(lo, hi), kSettings).value(); }

double volume_of(const Solid& s) {
  const auto m = mass_properties(s);
  REQUIRE(m.ok());
  return m->volume;
}
}  // namespace

TEST_CASE("booleans of overlapping boxes match analytic volumes", "[kernel][boolean]") {
  // Two 10 mm cubes overlapping in a 5 x 10 x 10 slab.
  const Solid a = box({0, 0, 0}, {10, 10, 10});
  const Solid b = box({5, 0, 0}, {15, 10, 10});

  const auto u = unite(a, b);
  REQUIRE(u.ok());
  CHECK(volume_of(*u) == Approx(1500.0).epsilon(kRel));
  CHECK(u->solid_count() == 1);
  // SimplifyResult merges the split coplanar faces: the union is a plain box.
  CHECK(faces(*u).size() == 6);

  const auto c = cut(a, b);
  REQUIRE(c.ok());
  CHECK(volume_of(*c) == Approx(500.0).epsilon(kRel));

  const auto i = common(a, b);
  REQUIRE(i.ok());
  CHECK(volume_of(*i) == Approx(500.0).epsilon(kRel));
  CHECK(mass_properties(*i)->center_of_mass.x == Approx(7.5).epsilon(kRel));
}

TEST_CASE("a cylinder cut from a box removes its volume", "[kernel][boolean]") {
  const Solid block = box({-20, -20, 0}, {20, 20, 10});
  const auto hole = make_cylinder({{0, 0, -1}, {0, 0, 1}}, 5, 12, kSettings);
  REQUIRE(hole.ok());
  const auto drilled = cut(block, *hole);
  REQUIRE(drilled.ok());
  CHECK(volume_of(*drilled) == Approx(40.0 * 40 * 10 - kPi * 25 * 10).epsilon(kRel));
  CHECK(classify(*drilled, {0, 0, 5}, kSettings) == PointClass::Outside);
  CHECK(classify(*drilled, {15, 15, 5}, kSettings) == PointClass::Inside);
}

TEST_CASE("booleans report empty and disjoint cases", "[kernel][boolean]") {
  const Solid a = box({0, 0, 0}, {10, 10, 10});
  const Solid far = box({20, 0, 0}, {30, 10, 10});
  const Solid big = box({-1, -1, -1}, {11, 11, 11});

  const auto none = common(a, far);
  REQUIRE(none.ok());
  CHECK(none->is_empty());

  const auto two = unite(a, far);
  REQUIRE(two.ok());
  CHECK(two->solid_count() == 2);
  CHECK(volume_of(*two) == Approx(2000.0).epsilon(kRel));

  CHECK(cut(a, big).error().code == "kernel.boolean.empty_result");
  CHECK(unite(a, Solid{}).error().code == "kernel.boolean.empty_input");
  CHECK(cut(Solid{}, a).error().code == "kernel.boolean.empty_input");
}

TEST_CASE("a cut can split a solid into lumps", "[kernel][boolean]") {
  const Solid bar = box({0, 0, 0}, {30, 10, 10});
  const Solid slab = box({10, -1, -1}, {20, 11, 11});
  const auto pieces = cut(bar, slab);
  REQUIRE(pieces.ok());
  CHECK(pieces->solid_count() == 2);
  CHECK(solids(*pieces).size() == 2);
  CHECK(volume_of(*pieces) == Approx(2000.0).epsilon(kRel));
}

TEST_CASE("rigid transforms move solids exactly", "[kernel][transform]") {
  const Solid a = box({0, 0, 0}, {2, 4, 6});
  const auto t = dtransform::translate({100, -50, 25}) * dtransform::rotate({0, 0, 1}, kPi / 2);
  const auto moved = transformed(a, t);
  REQUIRE(moved.ok());
  const auto m = mass_properties(*moved);
  REQUIRE(m.ok());
  CHECK(m->volume == Approx(48.0).epsilon(kRel));
  // The centre (1, 2, 3) rotates to (-2, 1, 3), then translates.
  CHECK(vs::core::approx_equal(m->center_of_mass, dvec3{98, -49, 28}, 1e-9));
  CHECK(faces(*moved).size() == 6);
}

TEST_CASE("scales change volume by the determinant", "[kernel][transform]") {
  const auto ball = make_sphere({0, 0, 0}, 3, kSettings);
  REQUIRE(ball.ok());
  const double v = 4.0 / 3.0 * kPi * 27;

  const auto uniform = transformed(*ball, dtransform::scale(2.0));
  REQUIRE(uniform.ok());
  CHECK(volume_of(*uniform) == Approx(8 * v).epsilon(kRel));

  // A non-uniform scale turns the sphere into an ellipsoid (B-spline faces),
  // so the volume is only as exact as the approximation.
  const auto stretched = transformed(*ball, dtransform::scale({2.0, 1.0, 1.0}));
  REQUIRE(stretched.ok());
  CHECK(volume_of(*stretched) == Approx(2 * v).epsilon(1e-6));

  CHECK(transformed(*ball, dtransform::scale({1.0, 0.0, 1.0})).error().code ==
        "kernel.transform.singular");
}

TEST_CASE("fillets round edges by the analytic amount", "[kernel][fillet]") {
  const Solid cube = box({0, 0, 0}, {20, 20, 20});
  // One edge parallel to z: the one at x = 0, y = 0.
  std::vector<Edge> picked;
  for (const Edge& e : edges(cube)) {
    const auto bb = bounding_box(e);
    if (bb.size().z > 10 && bb.max.x < 1e-9 && bb.max.y < 1e-9) picked.push_back(e);
  }
  REQUIRE(picked.size() == 1);

  const double r = 4;
  const auto rounded = fillet(cube, picked, r);
  REQUIRE(rounded.ok());
  CHECK(volume_of(*rounded) == Approx(8000 - (1 - kPi / 4) * r * r * 20).epsilon(kRel));

  SECTION("a radius too large for the faces fails with a fallback message") {
    const auto failed = fillet(cube, picked, 25);
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().code == "kernel.fillet.failed");
    CHECK(failed.error().message.find("smaller") != std::string::npos);
  }
  SECTION("radius and edge checks") {
    CHECK(fillet(cube, picked, 0).error().code == "kernel.fillet.invalid_radius");
    const Solid other = box({50, 50, 50}, {60, 60, 60});
    const auto foreign = edges(other);
    CHECK(fillet(cube, foreign, 1).error().code == "kernel.fillet.foreign_edge");
  }
}

#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/kernel.hpp"

using namespace vs::kernel;
using Catch::Approx;
using vs::core::approx_equal;
using vs::core::dbox3;
using vs::core::dvec3;
using vs::core::ProductSettings;

namespace {
const ProductSettings kSettings = ProductSettings::defaults();
constexpr double kPi = std::numbers::pi;
constexpr double kRel = 1e-9;

Solid box(dvec3 lo, dvec3 hi) { return make_box(dbox3::from_min_max(lo, hi), kSettings).value(); }
}  // namespace

TEST_CASE("mass properties of a box", "[kernel][queries]") {
  const Solid b = box({10, 20, 30}, {14, 26, 38});
  const auto m = mass_properties(b);
  REQUIRE(m.ok());
  CHECK(m->volume == Approx(4.0 * 6 * 8).epsilon(kRel));
  CHECK(approx_equal(m->center_of_mass, dvec3{12, 23, 34}, 1e-9));
  CHECK(surface_area(b) == Approx(2.0 * (4 * 6 + 6 * 8 + 4 * 8)).epsilon(kRel));
  CHECK(mass_properties(Solid{}).error().code == "kernel.mass.empty");
}

TEST_CASE("centre of gravity of a hemisphere", "[kernel][queries]") {
  // Keep the upper half of a sphere: its centroid sits 3r/8 above the base.
  const double r = 8;
  const auto ball = make_sphere({0, 0, 0}, r, kSettings);
  REQUIRE(ball.ok());
  const auto half = common(*ball, box({-10, -10, 0}, {10, 10, 10}));
  REQUIRE(half.ok());
  const auto m = mass_properties(*half);
  REQUIRE(m.ok());
  CHECK(m->volume == Approx(2.0 / 3.0 * kPi * r * r * r).epsilon(kRel));
  CHECK(approx_equal(m->center_of_mass, dvec3{0, 0, 3 * r / 8}, 1e-9));
}

TEST_CASE("bounding boxes are tight", "[kernel][queries]") {
  const auto ball = make_sphere({1, 2, 3}, 5, kSettings);
  REQUIRE(ball.ok());
  const dbox3 bb = bounding_box(*ball);
  CHECK(approx_equal(bb.min, dvec3{-4, -3, -2}, 1e-6));
  CHECK(approx_equal(bb.max, dvec3{6, 7, 8}, 1e-6));
  CHECK(bounding_box(Shape{}).is_empty());
}

TEST_CASE("minimum distance between solids", "[kernel][queries]") {
  const Solid a = box({0, 0, 0}, {10, 10, 10});

  SECTION("apart") {
    const auto d = min_distance(a, box({15, 0, 0}, {20, 10, 10}));
    REQUIRE(d.ok());
    CHECK(d->distance == Approx(5.0).epsilon(kRel));
    CHECK(d->point_on_a.x == Approx(10.0));
    CHECK(d->point_on_b.x == Approx(15.0));
  }
  SECTION("diagonally apart: corner to sphere") {
    const auto ball = make_sphere({20, 20, 20}, 5, kSettings);
    REQUIRE(ball.ok());
    const auto d = min_distance(a, *ball);
    REQUIRE(d.ok());
    CHECK(d->distance == Approx(std::sqrt(300.0) - 5).epsilon(1e-9));
  }
  SECTION("overlapping") {
    const auto d = min_distance(a, box({5, 5, 5}, {15, 15, 15}));
    REQUIRE(d.ok());
    CHECK(d->distance == Approx(0.0).margin(1e-12));
  }
  SECTION("one inside the other") {
    const auto d = min_distance(a, box({2, 2, 2}, {3, 3, 3}));
    REQUIRE(d.ok());
    CHECK(d->distance == 0.0);
  }
  SECTION("empty input") {
    CHECK(min_distance(a, Shape{}).error().code == "kernel.distance.empty_input");
  }
}

TEST_CASE("points classify against solids within the tolerance", "[kernel][queries]") {
  const Solid a = box({0, 0, 0}, {10, 10, 10});
  const double tol = kSettings.linear_tolerance_mm;
  CHECK(classify(a, {5, 5, 5}, kSettings) == PointClass::Inside);
  CHECK(classify(a, {15, 5, 5}, kSettings) == PointClass::Outside);
  CHECK(classify(a, {10, 5, 5}, kSettings) == PointClass::OnBoundary);
  CHECK(classify(a, {10 + tol / 2, 5, 5}, kSettings) == PointClass::OnBoundary);
  CHECK(classify(a, {10 + 10 * tol, 5, 5}, kSettings) == PointClass::Outside);

  // A compound of two lumps.
  const auto two = unite(a, box({20, 0, 0}, {30, 10, 10}));
  REQUIRE(two.ok());
  CHECK(classify(*two, {25, 5, 5}, kSettings) == PointClass::Inside);
  CHECK(classify(*two, {15, 5, 5}, kSettings) == PointClass::Outside);
}

TEST_CASE("topology queries count each sub-shape once", "[kernel][queries]") {
  const Solid a = box({0, 0, 0}, {3, 4, 5});
  CHECK(faces(a).size() == 6);
  CHECK(edges(a).size() == 12);
  CHECK(vertex_count(a) == 8);
  CHECK(solids(a).size() == 1);

  double total = 0;
  for (const Edge& e : edges(a)) total += length(e);
  CHECK(total == Approx(4.0 * (3 + 4 + 5)).epsilon(kRel));

  // The traversal order is deterministic: the same solid lists the same
  // faces in the same order.
  const auto f1 = faces(a);
  const auto f2 = faces(a);
  for (std::size_t i = 0; i < f1.size(); ++i) CHECK(f1[i].is_same(f2[i]));
}

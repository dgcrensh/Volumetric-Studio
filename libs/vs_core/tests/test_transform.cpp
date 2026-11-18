#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <numbers>

#include "vs/core/transform.hpp"
#include "vs/core/units.hpp"

using namespace vs::core;

namespace {
constexpr double kEps = 1e-12;
}

TEST_CASE("translate, scale and rotate", "[transform]") {
  const auto t = dtransform::translate({1, 2, 3});
  CHECK(t.apply_point({0, 0, 0}) == dvec3{1, 2, 3});
  CHECK(t.apply_vector({1, 0, 0}) == dvec3{1, 0, 0});  // vectors ignore translation

  CHECK(dtransform::scale(2.0).apply_point({1, 2, 3}) == dvec3{2, 4, 6});

  const auto r = dtransform::rotate({0, 0, 1}, degrees(90));
  CHECK(approx_equal(r.apply_point({1, 0, 0}), dvec3{0, 1, 0}, kEps));  // right handed
  CHECK(approx_equal(r.apply_point({0, 1, 0}), dvec3{-1, 0, 0}, kEps));

  const auto rx = dtransform::rotate({1, 0, 0}, degrees(90));
  CHECK(approx_equal(rx.apply_point({0, 1, 0}), dvec3{0, 0, 1}, kEps));

  // Axis need not be unit length; zero axis is the identity.
  CHECK(approx_equal(dtransform::rotate({0, 0, 5}, degrees(90)).apply_point({1, 0, 0}),
                     dvec3{0, 1, 0}, kEps));
  CHECK(dtransform::rotate({0, 0, 0}, 1.0) == dtransform::identity());
}

TEST_CASE("composition applies the right operand first", "[transform]") {
  const auto move = dtransform::translate({10, 0, 0});
  const auto turn = dtransform::rotate({0, 0, 1}, degrees(90));
  // Turn first, then move.
  CHECK(approx_equal((move * turn).apply_point({1, 0, 0}), dvec3{10, 1, 0}, kEps));
  // Move first, then turn.
  CHECK(approx_equal((turn * move).apply_point({1, 0, 0}), dvec3{0, 11, 0}, kEps));
}

TEST_CASE("inverse round-trips", "[transform]") {
  const auto t = dtransform::translate({5, -3, 2}) *
                 dtransform::rotate({1, 2, 3}, 0.7) * dtransform::scale({2, 3, 4});
  const auto inv = inverse(t);
  REQUIRE(inv.has_value());
  const dvec3 p{0.25, -7.5, 100};
  CHECK(approx_equal(inv->apply_point(t.apply_point(p)), p, 1e-9));
  CHECK(approx_equal(*inv * t, dtransform::identity(), 1e-12));

  CHECK_FALSE(inverse(dtransform::scale({1, 0, 1})).has_value());
}

TEST_CASE("normals use the inverse transpose", "[transform]") {
  // Squash x by 2: the normal of the plane x + y = 0 must stay perpendicular.
  const auto t = dtransform::scale({2, 1, 1});
  const dvec3 tangent{1, -1, 0};
  const auto n = t.apply_normal(normalize(dvec3{1, 1, 0}));
  REQUIRE(n.has_value());
  CHECK(std::abs(dot(*n, t.apply_vector(tangent))) < kEps);
  CHECK(std::abs(length(*n) - 1.0) < kEps);
}

TEST_CASE("to_matrix agrees with apply_point", "[transform]") {
  const auto t = dtransform::translate({1, 2, 3}) * dtransform::rotate({0, 1, 0}, 0.3);
  const dvec3 p{4, 5, 6};
  const dvec4 viaMatrix = t.to_matrix() * dvec4(p, 1.0);
  CHECK(approx_equal(viaMatrix.xyz(), t.apply_point(p), kEps));
  CHECK(viaMatrix.w == 1.0);
}

TEST_CASE("transform precision conversions", "[transform]") {
  const auto t = dtransform::translate({1, 2, 3});
  CHECK(to_fp64(to_fp32(t)) == t);
}

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include "vs/core/vec.hpp"

using namespace vs::core;
using Catch::Approx;

TEST_CASE("vector arithmetic", "[vec]") {
  const dvec3 a{1, 2, 3};
  const dvec3 b{4, 5, 6};
  CHECK(a + b == dvec3{5, 7, 9});
  CHECK(b - a == dvec3{3, 3, 3});
  CHECK(a * b == dvec3{4, 10, 18});
  CHECK(a * 2.0 == dvec3{2, 4, 6});
  CHECK(2.0 * a == dvec3{2, 4, 6});
  CHECK(b / 2.0 == dvec3{2, 2.5, 3});
  CHECK(-a == dvec3{-1, -2, -3});

  dvec3 c = a;
  c += b;
  CHECK(c == dvec3{5, 7, 9});
  c -= b;
  CHECK(c == a);
}

TEST_CASE("dot, cross, length and normalize", "[vec]") {
  const dvec3 x{1, 0, 0};
  const dvec3 y{0, 1, 0};
  CHECK(dot(x, y) == 0.0);
  CHECK(cross(x, y) == dvec3{0, 0, 1});
  CHECK(cross(dvec2{1, 0}, dvec2{0, 1}) == 1.0);
  CHECK(length(dvec3{3, 4, 0}) == 5.0);
  CHECK(length_squared(dvec3{3, 4, 0}) == 25.0);
  CHECK(distance(dvec3{1, 1, 1}, dvec3{1, 1, 4}) == 3.0);
  CHECK(length(normalize(dvec3{3, 4, 12})) == Approx(1.0));
  CHECK(normalize(dvec3{}) == dvec3{});
  CHECK(dot(dvec4{1, 2, 3, 4}, dvec4{1, 1, 1, 1}) == 10.0);
}

TEST_CASE("component-wise min, max and indexing", "[vec]") {
  CHECK(min(dvec3{1, 5, 3}, dvec3{4, 2, 6}) == dvec3{1, 2, 3});
  CHECK(max(dvec3{1, 5, 3}, dvec3{4, 2, 6}) == dvec3{4, 5, 6});
  dvec3 v{7, 8, 9};
  CHECK(v[0] == 7);
  CHECK(v[2] == 9);
  v[1] = 1;
  CHECK(v.y == 1);
  CHECK(dvec4(v, 1.0).xyz() == v);
}

TEST_CASE("precision conversions are explicit", "[vec]") {
  // FP32 and FP64 vectors are unrelated types: no implicit conversion.
  STATIC_REQUIRE_FALSE(std::is_convertible_v<dvec3, fvec3>);
  STATIC_REQUIRE_FALSE(std::is_convertible_v<fvec3, dvec3>);

  const dvec3 d{0.1, 1.0e10, -3.0};
  const fvec3 f = to_fp32(d);
  CHECK(f.x == 0.1f);
  CHECK(f.y == 1.0e10f);
  CHECK(to_fp64(f) == dvec3{static_cast<double>(0.1f), static_cast<double>(1.0e10f), -3.0});
}

TEST_CASE("approx_equal", "[vec]") {
  CHECK(approx_equal(dvec3{1, 2, 3}, dvec3{1 + 1e-7, 2, 3}, 1e-6));
  CHECK_FALSE(approx_equal(dvec3{1, 2, 3}, dvec3{1 + 1e-5, 2, 3}, 1e-6));
}

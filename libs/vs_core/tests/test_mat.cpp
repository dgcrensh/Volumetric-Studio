#include <catch2/catch_test_macros.hpp>

#include "vs/core/mat.hpp"

using namespace vs::core;

TEST_CASE("matrix layout is column major", "[mat]") {
  const dmat3 m = dmat3::from_columns({1, 2, 3}, {4, 5, 6}, {7, 8, 9});
  CHECK(m(0, 1) == 4);  // row 0, column 1
  CHECK(m(2, 0) == 3);
  CHECK(m[2] == dvec3{7, 8, 9});
  CHECK(transpose(m)(0, 1) == 2);
}

TEST_CASE("matrix products", "[mat]") {
  const dmat3 m = dmat3::from_columns({1, 0, 0}, {0, 2, 0}, {1, 0, 3});
  CHECK(m * dvec3{1, 1, 1} == dvec3{2, 2, 3});
  CHECK(dmat3::identity() * m == m);
  CHECK(m * dmat3::identity() == m);

  const dmat4 t = dmat4::from_columns({1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {5, 6, 7, 1});
  CHECK(t * dvec4{1, 1, 1, 1} == dvec4{6, 7, 8, 1});
  CHECK(t * dvec4{1, 1, 1, 0} == dvec4{1, 1, 1, 0});
}

TEST_CASE("3x3 determinant and inverse", "[mat]") {
  const dmat3 m = dmat3::from_columns({2, 0, 1}, {1, 3, 0}, {0, 1, 4});
  CHECK(determinant(m) == 25.0);
  const auto inv = inverse(m);
  REQUIRE(inv.has_value());
  CHECK(approx_equal(*inv * m, dmat3::identity(), 1e-12));
  CHECK(approx_equal(m * *inv, dmat3::identity(), 1e-12));

  const dmat3 singular = dmat3::from_columns({1, 2, 3}, {2, 4, 6}, {0, 0, 1});
  CHECK_FALSE(inverse(singular).has_value());
}

TEST_CASE("4x4 inverse", "[mat]") {
  const dmat4 m =
      dmat4::from_columns({2, 0, 0, 0}, {0, 0, 3, 0}, {0, -1, 0, 0}, {10, 20, 30, 1});
  const auto inv = inverse(m);
  REQUIRE(inv.has_value());
  CHECK(approx_equal(*inv * m, dmat4::identity(), 1e-12));
  CHECK(approx_equal(m * *inv, dmat4::identity(), 1e-12));

  dmat4 singular = dmat4::identity();
  singular[3] = dvec4{0, 0, 0, 0};
  CHECK_FALSE(inverse(singular).has_value());
}

TEST_CASE("matrix precision conversions", "[mat]") {
  const dmat4 m = dmat4::identity();
  CHECK(to_fp64(to_fp32(m)) == m);
}

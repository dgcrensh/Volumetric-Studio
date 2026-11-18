#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "vs/core/box.hpp"
#include "vs/core/units.hpp"

using namespace vs::core;

TEST_CASE("empty boxes", "[box]") {
  const dbox3 e;
  CHECK(e.is_empty());
  CHECK(e.size() == dvec3{});
  CHECK(e.volume() == 0.0);
  CHECK_FALSE(e.contains(dvec3{}));
  CHECK_FALSE(e.intersects(dbox3::from_min_max({-1, -1, -1}, {1, 1, 1})));

  dbox3 b;
  b.expand(dvec3{1, 2, 3});
  CHECK_FALSE(b.is_empty());
  CHECK(b.min == dvec3{1, 2, 3});
  CHECK(b.max == dvec3{1, 2, 3});
}

TEST_CASE("expanding, size and centre", "[box]") {
  dbox3 b;
  b.expand(dvec3{-1, 0, 2}).expand(dvec3{3, 4, -2});
  CHECK(b.min == dvec3{-1, 0, -2});
  CHECK(b.max == dvec3{3, 4, 2});
  CHECK(b.size() == dvec3{4, 4, 4});
  CHECK(b.center() == dvec3{1, 2, 0});
  CHECK(b.volume() == 64.0);

  dbox3 c;
  c.expand(b).expand(dbox3{});
  CHECK(c == b);
}

TEST_CASE("containment and intersection with tolerance", "[box]") {
  const auto b = dbox3::from_min_max({0, 0, 0}, {1, 1, 1});
  CHECK(b.contains(dvec3{1, 1, 1}));  // closed
  CHECK_FALSE(b.contains(dvec3{1 + 1e-7, 0.5, 0.5}));
  CHECK(b.contains(dvec3{1 + 1e-7, 0.5, 0.5}, 1e-6));

  const auto touching = dbox3::from_min_max({1, 0, 0}, {2, 1, 1});
  const auto apart = dbox3::from_min_max({1 + 1e-7, 0, 0}, {2, 1, 1});
  CHECK(b.intersects(touching));
  CHECK_FALSE(b.intersects(apart));
  CHECK(b.intersects(apart, 1e-6));

  CHECK(b.contains(dbox3::from_min_max({0.2, 0.2, 0.2}, {0.8, 0.8, 0.8})));
  CHECK_FALSE(b.contains(dbox3{}));

  const auto overlap = intersection(b, dbox3::from_min_max({0.5, 0.5, 0.5}, {3, 3, 3}));
  CHECK(overlap == dbox3::from_min_max({0.5, 0.5, 0.5}, {1, 1, 1}));
  CHECK(intersection(b, dbox3::from_min_max({2, 2, 2}, {3, 3, 3})).is_empty());
}

TEST_CASE("transformed boxes enclose the transformed corners", "[box]") {
  const auto b = dbox3::from_min_max({-1, -1, -1}, {1, 1, 1});
  const auto rotated = b.transformed(dtransform::rotate({0, 0, 1}, degrees(45)));
  const double r = std::sqrt(2.0);
  CHECK(approx_equal(rotated.max, dvec3{r, r, 1}, 1e-12));
  CHECK(approx_equal(rotated.min, dvec3{-r, -r, -1}, 1e-12));

  CHECK(b.inflated(0.5) == dbox3::from_min_max({-1.5, -1.5, -1.5}, {1.5, 1.5, 1.5}));
  CHECK(to_fp64(to_fp32(b)) == b);
  CHECK(to_fp32(dbox3{}).is_empty());
}

TEST_CASE("all empty boxes hash alike", "[box][hash]") {
  dbox3 odd;
  odd.min = {5, 5, 5};
  odd.max = {1, 1, 1};
  CHECK(odd.is_empty());
  CHECK(hash_of(odd) == hash_of(dbox3{}));
}

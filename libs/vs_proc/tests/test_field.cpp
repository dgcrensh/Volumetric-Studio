#include <cmath>
#include <random>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "scenes.hpp"
#include "vs/eval/thread_pool.hpp"
#include "vs/proc/field.hpp"

using namespace vs;
using namespace vs::proc;
using core::dvec3;
using Catch::Matchers::WithinAbs;

TEST_CASE("Field primitives match their formulas", "[proc][field]") {
  const auto sphere = sphere_field({1, 2, 3}, 4);
  CHECK_THAT(sphere->eval({1, 2, 3}), WithinAbs(-4.0, 1e-15));
  CHECK_THAT(sphere->eval({1, 2, 10}), WithinAbs(3.0, 1e-15));

  const auto box = box_field({0, 0, 0}, {4, 6, 8});
  CHECK_THAT(box->eval({0, 0, 0}), WithinAbs(-2.0, 1e-15));
  CHECK_THAT(box->eval({5, 0, 0}), WithinAbs(3.0, 1e-15));
  CHECK_THAT(box->eval({5, 7, 0}), WithinAbs(5.0, 1e-15));  // 3-4-5 from the edge
  CHECK_THAT(box->eval({0, 0, 3.5}), WithinAbs(-0.5, 1e-15));

  const auto cyl = cylinder_field({0, 0, 0}, {0, 0, 2}, 3, 10);
  CHECK_THAT(cyl->eval({0, 0, 0}), WithinAbs(-3.0, 1e-15));
  CHECK_THAT(cyl->eval({5, 0, 0}), WithinAbs(2.0, 1e-15));
  CHECK_THAT(cyl->eval({0, 0, 7}), WithinAbs(2.0, 1e-15));
  CHECK_THAT(cyl->eval({7, 0, 8}), WithinAbs(5.0, 1e-15));

  const auto plane = plane_field({0, 0, 1}, {0, 0, 5});
  CHECK_THAT(plane->eval({3, 3, -2}), WithinAbs(3.0, 1e-15));
  const auto half = half_space_field({0, 0, 1}, {0, 0, 5});
  CHECK_THAT(half->eval({3, 3, -2}), WithinAbs(-3.0, 1e-15));
  CHECK_THAT(half->eval({3, 3, 4}), WithinAbs(3.0, 1e-15));
  CHECK(constant_field(2.5)->eval({9, 9, 9}) == 2.5);
}

TEST_CASE("Field booleans and maths", "[proc][field]") {
  const auto a = half_space_field({0, 0, 0}, {1, 0, 0});  // x
  const auto b = half_space_field({0, 0, 0}, {0, 1, 0});  // y
  const dvec3 p{3, -2, 0};
  CHECK(union_field(a, b)->eval(p) == -2.0);
  CHECK(intersection_field(a, b)->eval(p) == 3.0);
  CHECK(difference_field(a, b)->eval(p) == 3.0);
  CHECK(offset_field(a, 1)->eval(p) == 2.0);
  CHECK(shell_field(a, 1)->eval({-0.5, 0, 0}) == -0.5);
  CHECK(shell_field(a, 1)->eval({-3, 0, 0}) == 2.0);
  CHECK(negate_field(a)->eval(p) == -3.0);
  CHECK(abs_field(b)->eval(p) == 2.0);
  CHECK(add_field(a, b)->eval(p) == 1.0);
  CHECK(multiply_field(a, b)->eval(p) == -6.0);
  CHECK(scale_field(a, 2)->eval(p) == 6.0);

  // Smooth booleans equal the sharp ones away from the blend, and stay
  // below (union) or above (intersection) them inside it.
  CHECK(smooth_min(1, 5, 1) == 1.0);
  CHECK(smooth_min(1, 1, 2) < 1.0);
  CHECK(smooth_max(1, 1, 2) > 1.0);
  CHECK(smooth_min(1, 1, 0) == 1.0);
  CHECK(smooth_union_field(a, b, 1)->eval({5, -5, 0}) == -5.0);
  CHECK(smooth_difference_field(a, b, 1)->eval({5, 5, 0}) == 5.0);
  CHECK(smooth_intersection_field(a, b, 1)->eval({0, 0, 0}) > 0.0);
}

TEST_CASE("Equal fields hash equal; different ones don't", "[proc][field]") {
  const auto one = union_field(sphere_field({0, 0, 0}, 1), box_field({1, 1, 1}, {2, 2, 2}));
  const auto two = union_field(sphere_field({0, 0, 0}, 1), box_field({1, 1, 1}, {2, 2, 2}));
  CHECK(one != two);
  CHECK(one->hash() == two->hash());
  CHECK(hash_value(Value(one)) == hash_value(Value(two)));
  CHECK(union_field(box_field({1, 1, 1}, {2, 2, 2}), sphere_field({0, 0, 0}, 1))->hash() !=
        one->hash());
  CHECK(sphere_field({0, 0, 0}, 1.0)->hash() != sphere_field({0, 0, 0}, 1.0000000001)->hash());
}

TEST_CASE("Grids cover the box", "[proc][field][sample]") {
  const auto box = core::dbox3::from_min_max({0, 0, 0}, {40, 30, 20});
  const auto g = grid_covering(box, 1.0);
  REQUIRE(g);
  CHECK(g->nx == 41);
  CHECK(g->ny == 31);
  CHECK(g->nz == 21);
  CHECK(g->point(40, 30, 20) == dvec3{40, 30, 20});
  const auto automatic = grid_covering(box, 0.0);
  REQUIRE(automatic);
  CHECK(automatic->cell == 40.0 / 64.0);
  CHECK(automatic->nx == 65);
  CHECK(grid_covering(core::dbox3{}, 1.0).error().code == "proc.sample.empty");
  CHECK(grid_covering(box, -1.0).error().code == "proc.sample.empty");
  CHECK(grid_covering(box, 0.01).error().code == "proc.sample.too_large");
}

TEST_CASE("Sampling is identical with any number of threads", "[proc][field][sample]") {
  const auto field = smooth_union_field(sphere_field({3, 4, 5}, 7),
                                        cylinder_field({10, 0, 0}, {1, 1, 0}, 2, 30), 1.5);
  const auto g = grid_covering(core::dbox3::from_min_max({-10, -10, -10}, {25, 20, 15}), 0.5);
  REQUIRE(g);
  const SampledField serial = sample(*field, *g);
  for (const std::size_t n : {1u, 4u, 16u}) {
    eval::ThreadPool pool(n);
    const SampledField parallel = sample(*field, *g, &pool);
    CHECK(parallel == serial);
    CHECK(parallel.hash() == serial.hash());
  }
  // Each sample is the field at its grid point, in FP32.
  CHECK(serial.at(3, 5, 7) == static_cast<float>(field->eval(g->point(3, 5, 7))));
}

TEST_CASE("A cancelled sampling says so", "[proc][field][sample]") {
  const auto g = grid_covering(core::dbox3::from_min_max({0, 0, 0}, {10, 10, 10}), 1.0);
  REQUIRE(g);
  eval::CancellationToken token;
  token.cancel();
  bool cancelled = false;
  (void)sample(*sphere_field({0, 0, 0}, 1), *g, nullptr, &token, &cancelled);
  CHECK(cancelled);
}

#include <cmath>
#include <limits>
#include <random>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "scenes.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/mesh/tessellate.hpp"
#include "vs/proc/distance.hpp"
#include "vs/proc/field.hpp"

using namespace vs;
using core::dvec3;
using Catch::Matchers::WithinAbs;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

proc::TriangleDistance closed_mesh(const kernel::Solid& solid, double deflection = 0.01) {
  mesh::TessellationOptions o;
  o.params.deflection = deflection;
  const auto vm = mesh::tessellate(solid, {core::SubjectKind::Volume, core::Id::random(), "s"}, {},
                                   kSettings, o);
  REQUIRE(vm.ok());
  return proc::TriangleDistance(vm.mesh, true);
}

}  // namespace

TEST_CASE("Signed distance to a tessellated box is the box's exact SDF", "[proc][distance]") {
  const dvec3 lo{-7, 2, 3}, hi{13, 17, 9};
  const auto box = kernel::make_box(core::dbox3::from_min_max(lo, hi), kSettings);
  REQUIRE(box);
  const auto d = closed_mesh(*box);
  CHECK(d.closed());
  std::mt19937_64 rng(42);
  std::uniform_real_distribution<double> u(-30.0, 30.0);
  for (int i = 0; i < 2000; ++i) {
    const dvec3 p{u(rng), u(rng), u(rng)};
    const double expected = scenes::box_distance(p, lo, hi);
    REQUIRE_THAT(d.signed_distance(p), WithinAbs(expected, 1e-9));
    REQUIRE_THAT(d.distance(p), WithinAbs(std::abs(expected), 1e-9));
  }
  // Points right next to edges and corners, inside and out.
  for (const dvec3 p : {dvec3{-7.001, 2.001, 5}, dvec3{-6.999, 1.999, 5}, dvec3{-6.999, 2.001, 3.001},
                        dvec3{13.001, 17.001, 9.001}, dvec3{12.999, 16.999, 8.999}}) {
    REQUIRE_THAT(d.signed_distance(p), WithinAbs(scenes::box_distance(p, lo, hi), 1e-9));
  }
}

TEST_CASE("Signed distance to a tessellated cylinder is within the deflection",
          "[proc][distance]") {
  const auto cyl = kernel::make_cylinder({{1, 2, -5}, {0, 0, 1}}, 8.0, 10.0, kSettings);
  REQUIRE(cyl);
  const auto d = closed_mesh(*cyl, 0.01);
  const auto exact = [](const dvec3& p) {
    const double r = std::sqrt((p.x - 1) * (p.x - 1) + (p.y - 2) * (p.y - 2)) - 8.0;
    const double h = std::abs(p.z - 0.0) - 5.0;
    const double ox = std::max(r, 0.0), oy = std::max(h, 0.0);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(r, h), 0.0);
  };
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<double> u(-15.0, 15.0);
  kernel::SolidClassifier inside(*cyl, kSettings);
  for (int i = 0; i < 1000; ++i) {
    const dvec3 p{u(rng), u(rng), u(rng)};
    const double e = exact(p);
    const double got = d.signed_distance(p);
    REQUIRE_THAT(got, WithinAbs(e, 0.02));
    if (std::abs(e) > 0.05) {
      REQUIRE((got < 0) == (inside.classify(p) == kernel::PointClass::Inside));
    }
  }
}

TEST_CASE("An open mesh gives unsigned distances", "[proc][distance]") {
  mesh::Mesh m;
  const auto a = m.add_vertex({0, 0, 0});
  const auto b = m.add_vertex({10, 0, 0});
  const auto c = m.add_vertex({10, 10, 0});
  const auto e = m.add_vertex({0, 10, 0});
  m.add_triangle(a, b, c, core::Id{});
  m.add_triangle(a, c, e, core::Id{});
  const proc::TriangleDistance d(m, false);
  CHECK_FALSE(d.closed());
  CHECK_THAT(d.signed_distance({5, 5, -3}), WithinAbs(3.0, 1e-12));
  CHECK_THAT(d.distance({5, 5, 3}), WithinAbs(3.0, 1e-12));
  CHECK_THAT(d.distance({13, 14, 0}), WithinAbs(5.0, 1e-12));
  CHECK(proc::TriangleDistance(m, false).hash() == d.hash());
  CHECK(proc::TriangleDistance(m, true).hash() != d.hash());
  CHECK(std::isinf(proc::TriangleDistance().distance({0, 0, 0})));
}

TEST_CASE("Each BVH leaf lists its triangles in increasing order (H3)", "[proc][distance]") {
  // Random triangles: nth_element() leaves the halves it splits in an
  // order the standard library chooses, which before H3 reached the leaves
  // (and so which of two equally near triangles a query found).
  mesh::Mesh m;
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<float> u(-10.0f, 10.0f);
  for (int t = 0; t < 3000; ++t) {
    const auto a = m.add_vertex({u(rng), u(rng), u(rng)});
    const auto b = m.add_vertex({u(rng), u(rng), u(rng)});
    const auto c = m.add_vertex({u(rng), u(rng), u(rng)});
    m.add_triangle(a, b, c, core::Id::null());
  }
  const proc::TriangleDistance d(m, false);
  REQUIRE(d.nodes().size() > 1000);
  int leaves = 0;
  for (const auto& n : d.nodes()) {
    if (n.count == 0) continue;
    ++leaves;
    CHECK(n.count <= proc::kBvhLeafSize);
    for (std::uint32_t i = n.first + 1; i < n.first + n.count; ++i) {
      CHECK(d.order()[i - 1] < d.order()[i]);
    }
  }
  CHECK(leaves > 500);
}

TEST_CASE("atan2_det() is libm's atan2 to within an ulp, from arithmetic alone (H3)",
          "[proc][distance]") {
  constexpr double kPi = 3.14159265358979311600;
  CHECK(proc::atan2_det(0.0, 0.0) == 0.0);
  CHECK(proc::atan2_det(0.0, 1.0) == 0.0);
  CHECK(proc::atan2_det(0.0, -1.0) == kPi);
  CHECK(proc::atan2_det(1.0, 0.0) == kPi / 2);
  CHECK(proc::atan2_det(-1.0, 0.0) == -kPi / 2);
  CHECK(proc::atan2_det(1.0, 1.0) == kPi / 4);
  CHECK(std::isnan(proc::atan2_det(std::numeric_limits<double>::quiet_NaN(), 1.0)));
  CHECK(std::isnan(proc::atan2_det(1.0, std::numeric_limits<double>::infinity())));
  std::mt19937_64 rng(11);
  std::uniform_real_distribution<double> u(-1.0, 1.0);
  std::uniform_real_distribution<double> e(-30.0, 30.0);
  for (int i = 0; i < 200000; ++i) {
    const double y = u(rng) * std::pow(10.0, e(rng));
    const double x = u(rng) * std::pow(10.0, e(rng));
    const double want = std::atan2(y, x);
    const double got = proc::atan2_det(y, x);
    const double ulp = std::nextafter(std::abs(want), 4.0) - std::abs(want);
    INFO("atan2(" << y << ", " << x << ")");
    REQUIRE(std::abs(got - want) <= ulp);
  }
}

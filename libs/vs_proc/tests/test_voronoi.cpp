// Voronoi walls (decision 0042): the field against a brute-force
// reference, its geometry, its digest, and the node's checks.

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "scenes.hpp"
#include "vs/proc/evaluate.hpp"
#include "vs/proc/field.hpp"
#include "vs/proc/field_layout.hpp"

using namespace vs;
using namespace vs::proc;
using core::dvec3;
using Catch::Approx;

namespace {

// splitmix64, so the test points are the same everywhere.
struct Random {
  std::uint64_t state;
  double next() {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);
    return static_cast<double>(z >> 11) * 0x1.0p-53;
  }
};

FrameSet seeds_at(const std::vector<dvec3>& points) {
  FrameSet set;
  for (const auto& p : points) {
    core::ftransform f;
    f.translation = core::to_fp32(p);
    set.frames.push_back(f);
  }
  return set;
}

// The definition, over every pair: nearest seed a, then the nearest
// bisector plane of a and any other seed, less half the thickness. Seeds
// go through FP32 exactly as the field takes them.
double brute(const FrameSet& set, double thickness, const dvec3& p) {
  std::vector<dvec3> s;
  for (const auto& f : set.frames) s.push_back(core::to_fp64(f.translation));
  const auto d2 = [](const dvec3& q, const dvec3& x) {
    const double dx = x.x - q.x;
    const double dy = x.y - q.y;
    const double dz = x.z - q.z;
    return dx * dx + dy * dy + dz * dz;
  };
  std::size_t a = 0;
  double best = d2(s[0], p);
  for (std::size_t i = 1; i < s.size(); ++i) {
    const double d = d2(s[i], p);
    if (d < best) {
      best = d;
      a = i;
    }
  }
  double wall = std::numeric_limits<double>::infinity();
  for (std::size_t b = 0; b < s.size(); ++b) {
    if (b == a) continue;
    const double w = (d2(s[b], p) - best) / (2.0 * std::sqrt(d2(s[b], s[a])));
    if (w < wall) wall = w;
  }
  return wall - 0.5 * thickness;
}

std::vector<dvec3> random_points(Random& r, std::size_t count, const dvec3& lo, const dvec3& size) {
  std::vector<dvec3> out;
  for (std::size_t i = 0; i < count; ++i) {
    const double x = r.next(), y = r.next(), z = r.next();
    out.push_back({lo.x + size.x * x, lo.y + size.y * y, lo.z + size.z * z});
  }
  return out;
}

// Evaluates a graph whose output is `field` of node `source`.
GraphEvaluation run(Graph& g, const Id& source) {
  const Id out = g.add("output");
  g.link(source, "field", out, "contents");
  EvaluateGraphOptions o;
  o.keep_outputs = true;
  return evaluate_graph(g, scenes::bare_inputs(), o);
}

}  // namespace

TEST_CASE("Voronoi walls equal the brute-force minimum bit for bit", "[proc][field][voronoi]") {
  struct Case {
    const char* name;
    std::vector<dvec3> seeds;
  };
  Random r{42};
  std::vector<Case> cases;
  cases.push_back({"two", {{0, 0, 0}, {10, 3, -2}}});
  cases.push_back({"three", {{0, 0, 0}, {10, 0, 0}, {5, 8, 1}}});
  cases.push_back({"fifty in a box", random_points(r, 50, {0, 0, 0}, {40, 30, 20})});
  cases.push_back({"two thousand in a box", random_points(r, 2000, {-20, -10, 0}, {40, 30, 20})});
  // A flat layer, as one layer of offset surface points makes, and a
  // cluster with a few far seeds: grids one cell thick, and lopsided ones.
  cases.push_back({"a flat layer", random_points(r, 400, {0, 0, 5}, {40, 30, 0})});
  {
    auto cluster = random_points(r, 300, {0, 0, 0}, {1, 1, 1});
    for (const auto& far : random_points(r, 5, {-50, -50, -50}, {100, 100, 100})) cluster.push_back(far);
    cases.push_back({"a cluster", cluster});
  }
  for (const auto& c : cases) {
    INFO(c.name);
    const FrameSet set = seeds_at(c.seeds);
    const Field f = voronoi_field(set, 0.8);
    REQUIRE(f->op() == FieldOp::Voronoi);
    // Points round the seeds and well outside them.
    dvec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    for (const auto& s : c.seeds) {
      lo = {std::min(lo.x, s.x), std::min(lo.y, s.y), std::min(lo.z, s.z)};
      hi = {std::max(hi.x, s.x), std::max(hi.y, s.y), std::max(hi.z, s.z)};
    }
    const dvec3 pad{10, 10, 10};
    const auto points = random_points(r, 2500, lo - pad, (hi - lo) + pad * 2.0);
    std::size_t differ = 0;
    for (const auto& p : points) {
      if (f->eval(p) != brute(set, 0.8, p)) ++differ;
    }
    CHECK(differ == 0);
    // And at the seeds themselves (half the thickness below the nearest
    // wall's distance).
    for (const auto& s : c.seeds) {
      const dvec3 at = core::to_fp64(core::to_fp32(s));
      CHECK(f->eval(at) == brute(set, 0.8, at));
    }
  }
}

TEST_CASE("Voronoi walls sit on the bisector planes", "[proc][field][voronoi]") {
  const Field f = voronoi_field(seeds_at({{0, 0, 0}, {10, 0, 0}}), 1.0);
  CHECK(f->eval({5, 3, -7}) == Approx(-0.5));  // on the wall's mid-plane
  CHECK(f->eval({5.5, 0, 0}) == Approx(0.0).margin(1e-12));
  CHECK(f->eval({4.5, 0, 0}) == Approx(0.0).margin(1e-12));
  CHECK(f->eval({2, 4, 1}) == Approx(2.5));
  CHECK(f->eval({-30, 0, 0}) == Approx(34.5));  // beyond the seeds

  // A cubic grid of seeds 10 apart: cells are cubes, walls on the planes
  // halfway between.
  std::vector<dvec3> grid;
  for (int z = 0; z < 4; ++z) {
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) grid.push_back({10.0 * x, 10.0 * y, 10.0 * z});
    }
  }
  const Field cubes = voronoi_field(seeds_at(grid), 2.0);
  CHECK(cubes->eval({15, 12, 13}) == Approx(-1.0));   // on x = 15
  CHECK(cubes->eval({12, 12, 13}) == Approx(1.0));    // nearest wall z = 15, 2 away
}

TEST_CASE("Voronoi fields hash by their seeds and thickness", "[proc][field][voronoi]") {
  const FrameSet a = seeds_at({{0, 0, 0}, {10, 0, 0}, {0, 7, 3}});
  CHECK(voronoi_field(a, 1.0)->hash() == voronoi_field(a, 1.0)->hash());
  CHECK(voronoi_field(a, 1.0)->hash() != voronoi_field(a, 1.5)->hash());
  const FrameSet b = seeds_at({{0, 0, 0}, {10, 0, 0}, {0, 7, 4}});
  CHECK(voronoi_field(a, 1.0)->hash() != voronoi_field(b, 1.0)->hash());
}

TEST_CASE("The Voronoi node checks its seeds and thickness", "[proc][nodes][voronoi]") {
  const auto with = [](const std::vector<dvec3>& points, double thickness) {
    Graph g;
    Id last;
    Id merged;
    for (std::size_t i = 0; i < points.size(); ++i) {
      const Id p = g.add("frames.point", "P" + std::to_string(i), {{"position", Setting(points[i])}});
      if (i == 0) {
        merged = p;
      } else {
        const Id m = g.add("frames.merge", "M" + std::to_string(i));
        g.link(merged, "frames", m, "a");
        g.link(p, "frames", m, "b");
        merged = m;
      }
    }
    const Id v = g.add("field.voronoi", "Cells", {{"thickness", Setting(thickness)}});
    g.link(merged, "frames", v, "frames");
    return run(g, v);
  };
  SECTION("evaluates") {
    const auto ev = with({{0, 0, 0}, {10, 0, 0}}, 1.0);
    INFO(scenes::codes(ev.diagnostics));
    REQUIRE(ev.ok());
    CHECK((*ev.contents)->op() == FieldOp::Voronoi);
    CHECK((*ev.contents)->eval({5, 0, 0}) == Approx(-0.5));
  }
  SECTION("one seed is too few") {
    const auto ev = with({{0, 0, 0}}, 1.0);
    CHECK_FALSE(ev.ok());
    CHECK_FALSE(ev.diagnostics.with_code("proc.node.infeasible").empty());
  }
  SECTION("two seeds at one place") {
    const auto ev = with({{0, 0, 0}, {10, 0, 0}, {0, 0, 0}}, 1.0);
    CHECK_FALSE(ev.ok());
    CHECK_FALSE(ev.diagnostics.with_code("proc.node.infeasible").empty());
  }
  SECTION("walls thinner than the minimum feature size") {
    const auto ev = with({{0, 0, 0}, {10, 0, 0}}, 1e-9);
    CHECK_FALSE(ev.ok());
    CHECK_FALSE(ev.diagnostics.with_code("proc.node.infeasible").empty());
  }
}

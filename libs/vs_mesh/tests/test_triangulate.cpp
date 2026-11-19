#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <random>

#include <catch2/catch_test_macros.hpp>

#include "vs/mesh/triangulate.hpp"

using vs::core::dvec2;
using vs::mesh::Loop;
using vs::mesh::triangulate;
using vs::mesh::Triangle;

namespace {

double orient(const dvec2& a, const dvec2& b, const dvec2& c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Checks the promises of triangulate.hpp: counter-clockwise triangles that
// cover the polygon's area, every loop segment an edge of exactly one
// triangle, every interior edge shared by two triangles, every point used.
void check_triangulation(const std::vector<dvec2>& pts, const std::vector<Loop>& loops,
                         const std::vector<Triangle>& tris) {
  double area = 0;
  std::map<std::pair<std::uint32_t, std::uint32_t>, int> directed;
  std::vector<bool> used(pts.size(), false);
  for (const Triangle& t : tris) {
    const double o = orient(pts[t[0]], pts[t[1]], pts[t[2]]);
    REQUIRE(o > 0);
    area += o / 2;
    for (std::size_t k = 0; k < 3; ++k) {
      ++directed[{t[k], t[(k + 1) % 3]}];
      used[t[k]] = true;
    }
  }
  double expected = 0;
  std::map<std::pair<std::uint32_t, std::uint32_t>, bool> boundary;
  for (std::size_t l = 0; l < loops.size(); ++l) {
    expected += std::abs(vs::mesh::signed_double_area(pts, loops[l])) / 2 * (l == 0 ? 1 : -1);
    for (std::size_t i = 0; i < loops[l].size(); ++i) {
      const std::uint32_t a = loops[l][i], b = loops[l][(i + 1) % loops[l].size()];
      boundary[{std::min(a, b), std::max(a, b)}] = true;
    }
  }
  CHECK(std::abs(area - expected) <= 1e-9 * std::max(1.0, expected));
  for (const auto& [e, n] : directed) {
    REQUIRE(n == 1);  // no edge twice the same way
    const bool twin = directed.contains({e.second, e.first});
    const bool on_loop = boundary.contains({std::min(e.first, e.second), std::max(e.first, e.second)});
    CHECK(twin != on_loop);  // loop segments: one side only; others: both
  }
  for (const auto& [e, b] : boundary) {
    CHECK((directed.contains({e.first, e.second}) || directed.contains({e.second, e.first})));
  }
  CHECK(std::all_of(used.begin(), used.end(), [](bool u) { return u; }));
}

}  // namespace

TEST_CASE("A square triangulates into two triangles", "[mesh][triangulate]") {
  const std::vector<dvec2> pts{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  auto r = triangulate(pts, {{0, 1, 2, 3}});
  REQUIRE(r.ok());
  CHECK(r->size() == 2);
  check_triangulation(pts, {{0, 1, 2, 3}}, *r);
  // Clockwise input gives the same counter-clockwise triangles.
  auto cw = triangulate(pts, {{3, 2, 1, 0}});
  REQUIRE(cw.ok());
  check_triangulation(pts, {{3, 2, 1, 0}}, *cw);
}

TEST_CASE("Collinear points, holes and Steiner points", "[mesh][triangulate]") {
  // A 10 x 10 square with 1 mm divisions, a 2 x 2 hole, and interior points.
  std::vector<dvec2> pts;
  Loop outer, hole;
  const auto add = [&](dvec2 p) {
    outer.push_back(static_cast<std::uint32_t>(pts.size()));
    pts.push_back(p);
  };
  for (int i = 0; i < 10; ++i) add({i * 1.0, 0});
  for (int i = 0; i < 10; ++i) add({10, i * 1.0});
  for (int i = 0; i < 10; ++i) add({10 - i * 1.0, 10});
  for (int i = 0; i < 10; ++i) add({0, 10 - i * 1.0});
  for (const dvec2 p : {dvec2{4, 4}, dvec2{4, 6}, dvec2{6, 6}, dvec2{6, 4}}) {
    hole.push_back(static_cast<std::uint32_t>(pts.size()));
    pts.push_back(p);
  }
  for (int i = 1; i < 10; i += 2) {
    for (int j = 1; j < 10; j += 2) {
      if (i >= 3 && i <= 7 && j >= 3 && j <= 7) continue;
      pts.push_back({i + 0.25, j + 0.1});
    }
  }
  const std::vector<Loop> loops{outer, hole};
  auto r = triangulate(pts, loops);
  REQUIRE(r.ok());
  check_triangulation(pts, loops, *r);
  // Deterministic.
  auto again = triangulate(pts, loops);
  REQUIRE(again.ok());
  CHECK(*again == *r);
}

TEST_CASE("Bad input is refused with a code", "[mesh][triangulate]") {
  const std::vector<dvec2> bowtie{{0, 0}, {1, 1}, {1, 0}, {0, 1}};
  auto r = triangulate(bowtie, {{0, 1, 2, 3}});
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == "mesh.triangulate.self_intersecting");

  const std::vector<dvec2> dup{{0, 0}, {1, 0}, {1, 1}, {0, 1}, {1, 1}};
  r = triangulate(dup, {{0, 1, 2, 3}});
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == "mesh.triangulate.duplicate_point");

  const std::vector<dvec2> outside{{0, 0}, {1, 0}, {1, 1}, {0, 1}, {2, 2}};
  r = triangulate(outside, {{0, 1, 2, 3}});
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == "mesh.triangulate.steiner_outside");

  const std::vector<dvec2> hole_out{{0, 0}, {1, 0}, {1, 1}, {0, 1}, {2, 2}, {3, 2}, {3, 3}};
  r = triangulate(hole_out, {{0, 1, 2, 3}, {4, 5, 6}});
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == "mesh.triangulate.input");

  r = triangulate(bowtie, {{0, 1}});
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == "mesh.triangulate.input");

  // A hole touching the outer loop.
  const std::vector<dvec2> touch{{0, 0}, {4, 0}, {4, 4}, {0, 4}, {1, 0}, {2, 1}, {3, 1}};
  r = triangulate(touch, {{0, 1, 2, 3}, {4, 5, 6}});
  REQUIRE_FALSE(r.ok());
}

TEST_CASE("Random star polygons with holes triangulate correctly", "[mesh][triangulate][property]") {
  std::mt19937_64 rng(20261119);
  std::uniform_real_distribution<double> u(0, 1);
  for (int iteration = 0; iteration < 1000; ++iteration) {
    CAPTURE(iteration);
    std::vector<dvec2> pts;
    Loop outer;
    const int n = 8 + static_cast<int>(u(rng) * 60);
    const double scale = std::pow(10.0, u(rng) * 4 - 2);  // 0.01 to 100
    const dvec2 shift{(u(rng) - 0.5) * 1000, (u(rng) - 0.5) * 1000};
    for (int k = 0; k < n; ++k) {
      const double a = 2 * std::numbers::pi * (k + 0.8 * (u(rng) - 0.5)) / n;
      const double r = scale * (0.5 + 0.5 * u(rng));
      outer.push_back(static_cast<std::uint32_t>(pts.size()));
      pts.push_back({shift.x + r * std::cos(a), shift.y + r * std::sin(a)});
    }
    std::vector<Loop> loops{outer};
    // A small hole near the centre half the time.
    if (u(rng) < 0.5) {
      Loop hole;
      const double hr = scale * 0.1;
      for (int k = 0; k < 6; ++k) {
        const double a = -2 * std::numbers::pi * k / 6;
        hole.push_back(static_cast<std::uint32_t>(pts.size()));
        pts.push_back({shift.x + hr * std::cos(a), shift.y + hr * std::sin(a)});
      }
      loops.push_back(hole);
    }
    // Steiner points inside the ring between the hole and 0.3 x scale.
    const int steiner = static_cast<int>(u(rng) * 40);
    for (int k = 0; k < steiner; ++k) {
      const double a = 2 * std::numbers::pi * u(rng);
      const double r = scale * (0.15 + 0.15 * u(rng));
      pts.push_back({shift.x + r * std::cos(a), shift.y + r * std::sin(a)});
    }
    auto r = triangulate(pts, loops);
    REQUIRE(r.ok());
    check_triangulation(pts, loops, *r);
  }
}

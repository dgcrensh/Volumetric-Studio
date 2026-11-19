#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <tuple>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/kernel.hpp"

using namespace vs::kernel;
using Catch::Approx;
using vs::core::dbox3;
using vs::core::dvec3;
using vs::core::ProductSettings;

namespace {

const ProductSettings kSettings = ProductSettings::defaults();
constexpr double kPi = std::numbers::pi;

// Joins the faces' triangulations by where their points sit (vertex, edge
// point, or a face's own interior point), the way vs_mesh does, and checks
// the result is closed: every edge used once each way. Returns the
// enclosed volume.
double closed_volume(const Tessellation& t) {
  std::map<std::tuple<int, std::uint32_t, std::uint32_t>, std::uint32_t> ids;
  std::vector<dvec3> pts;
  const auto id_of = [&](std::size_t face, std::size_t point) {
    const TessFace& f = t.faces[face];
    const TessNode& n = f.nodes[point];
    std::tuple<int, std::uint32_t, std::uint32_t> key;
    dvec3 p = f.points[point];
    switch (n.kind) {
      case TessNode::Kind::Vertex:
        key = {0, n.index, 0};
        p = t.vertices[n.index];
        break;
      case TessNode::Kind::Edge:
        key = {1, n.index, n.k};
        p = t.edges[n.index].points[n.k];
        break;
      default:
        key = {2 + static_cast<int>(face), static_cast<std::uint32_t>(point), 0};
    }
    auto [it, added] = ids.try_emplace(key, static_cast<std::uint32_t>(pts.size()));
    if (added) pts.push_back(p);
    return it->second;
  };
  std::map<std::pair<std::uint32_t, std::uint32_t>, int> directed;
  double volume = 0;
  for (std::size_t f = 0; f < t.faces.size(); ++f) {
    for (const auto& tri : t.faces[f].triangles) {
      const std::uint32_t a = id_of(f, tri[0]), b = id_of(f, tri[1]), c = id_of(f, tri[2]);
      if (a == b || b == c || c == a) continue;  // at a pole
      ++directed[{a, b}];
      ++directed[{b, c}];
      ++directed[{c, a}];
      volume += vs::core::dot(pts[a], vs::core::cross(pts[b], pts[c])) / 6.0;
    }
  }
  for (const auto& [e, n] : directed) {
    INFO("edge " << e.first << "-" << e.second);
    CHECK(n == 1);
    CHECK(directed.count({e.second, e.first}) == 1);
  }
  return volume;
}

}  // namespace

TEST_CASE("Tessellating a box gives faces that join edge to edge", "[kernel][tessellate]") {
  const Solid b = make_box(dbox3::from_min_max({0, 0, 0}, {10, 20, 30}), kSettings).value();
  const auto t = tessellate(b, {}, kSettings);
  REQUIRE(t.ok());
  CHECK(t->vertices.size() == 8);
  CHECK(t->edges.size() == 12);
  REQUIRE(t->faces.size() == 6);
  for (const TessFace& f : t->faces) {
    REQUIRE(f.plane.has_value());
    CHECK(f.wires.size() == 1);
    CHECK(f.wires[0].size() == 4);
  }
  for (const TessEdge& e : t->edges) {
    CHECK(e.params.size() == e.points.size());
    CHECK(e.points.front() == t->vertices[e.first_vertex]);
    CHECK(e.points.back() == t->vertices[e.last_vertex]);
  }
  CHECK(closed_volume(*t) == Approx(6000.0).epsilon(1e-9));
}

TEST_CASE("Curved solids tessellate closed, seams and poles included", "[kernel][tessellate]") {
  SECTION("cylinder") {
    const Solid c = make_cylinder({{1, 2, 3}, {0, 0, 1}}, 5, 7, kSettings).value();
    const auto t = tessellate(c, {0.01, 0.2}, kSettings);
    REQUIRE(t.ok());
    // The side face's seam appears twice in its wire.
    bool seam_twice = false;
    for (const TessFace& f : t->faces) {
      std::map<std::uint32_t, int> uses;
      for (const auto& w : f.wires) {
        for (const TessEdgeUse& u : w) ++uses[u.edge];
      }
      for (const auto& [e, n] : uses) seam_twice |= n == 2;
    }
    CHECK(seam_twice);
    CHECK(closed_volume(*t) == Approx(kPi * 25 * 7).epsilon(2e-3));
  }
  SECTION("sphere") {
    const Solid s = make_sphere({0, 0, 0}, 4, kSettings).value();
    const auto t = tessellate(s, {0.01, 0.2}, kSettings);
    REQUIRE(t.ok());
    bool degenerate = false;
    for (const TessEdge& e : t->edges) degenerate |= e.degenerate;
    CHECK(degenerate);
    CHECK(closed_volume(*t) == Approx(4.0 / 3 * kPi * 64).epsilon(6e-3));
  }
  SECTION("cone") {
    const auto profile = make_face(
        Loop::polygon(std::vector<dvec3>{{0, 0, 0}, {6, 0, 0}, {0, 0, 9}}), kSettings);
    REQUIRE(profile.ok());
    const auto cone = make_revolution(*profile, {{0, 0, 0}, {0, 0, 1}}, 2 * kPi, kSettings);
    REQUIRE(cone.ok());
    const auto t = tessellate(*cone, {0.01, 0.2}, kSettings);
    REQUIRE(t.ok());
    CHECK(closed_volume(*t) == Approx(kPi * 36 * 9 / 3).epsilon(3e-3));
    // A revolution's planar face can lie on a left-handed plane; its normal
    // still points out of the material.
    for (const TessFace& f : t->faces) {
      if (f.plane) CHECK(f.plane->normal.z == Approx(-1.0));
    }
  }
  SECTION("filleted box") {
    const Solid b = make_box(dbox3::from_min_max({0, 0, 0}, {20, 20, 10}), kSettings).value();
    const auto f = fillet(b, edges(b), 2.0);
    REQUIRE(f.ok());
    const auto t = tessellate(*f, {0.01, 0.2}, kSettings);
    REQUIRE(t.ok());
    const double exact = mass_properties(*f)->volume;
    CHECK(closed_volume(*t) == Approx(exact).epsilon(3e-3));
  }
}

TEST_CASE("Points on edges and their (u, v)", "[kernel][tessellate]") {
  const Solid c = make_cylinder({{0, 0, 0}, {0, 0, 1}}, 5, 10, kSettings).value();
  const auto t = tessellate(c, {}, kSettings);
  REQUIRE(t.ok());
  for (const TessFace& f : t->faces) {
    for (const auto& w : f.wires) {
      for (const TessEdgeUse& u : w) {
        const TessEdge& e = t->edges[u.edge];
        if (e.degenerate) continue;
        for (std::size_t k = 0; k < e.params.size(); ++k) {
          const auto at = edge_parameter(e.edge, e.points[k], 1e-6);
          REQUIRE(at.has_value());
          CHECK(vs::core::length(edge_point(e.edge, *at) - e.points[k]) < 1e-9);
          const vs::core::dvec2 uv = edge_uv(f.face, u.oriented, e.params[k]);
          CHECK(std::isfinite(uv.x));
          CHECK(std::isfinite(uv.y));
        }
        // A point off the edge isn't on it.
        CHECK_FALSE(edge_parameter(e.edge, e.points[0] + dvec3{0, 0, 0.5} * 1e-3 + dvec3{7, 7, 0}, 1e-6)
                        .has_value());
      }
    }
  }
  CHECK(tessellate(c, {0, 0.2}, kSettings).error().code == "kernel.tessellate.params");
  CHECK(tessellate(Solid{}, {}, kSettings).error().code == "kernel.tessellate.empty");
}

#include <chrono>
#include <cmath>
#include <numbers>
#include <random>
#include <set>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "meshers.hpp"
#include "vs/core/settings.hpp"
#include "vs/mesh/seam.hpp"
#include "vs/mesh/topology.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using vs::core::dvec2;
using vs::core::dvec3;
using vs::core::Id;
using vs::core::SubjectKind;
using vs::core::SubjectRef;
using vs::mesh::make_seam;
using vs::mesh::OutlineLoop;
using vs::mesh::OutlineSegment;
using vs::mesh::Seam;
using vs::mesh::SeamOutline;

namespace {

constexpr double kPi = std::numbers::pi;
const auto kSettings = vs::core::ProductSettings::defaults();

SubjectRef surface(const char* name = "patch") {
  return {SubjectKind::ConnectionSurface, Id::derive(Id::null(), name), name};
}

// Checks what seam.hpp promises about any seam.
void check_seam(const Seam& seam) {
  // Orientation: outer counter-clockwise, holes clockwise.
  std::vector<dvec2> uv;
  for (const auto& v : seam.vertices) uv.push_back(v.uv);
  for (std::size_t l = 0; l < seam.loops.size(); ++l) {
    const double a = vs::mesh::signed_double_area(uv, seam.loops[l].vertices);
    CHECK((l == 0 ? a > 0 : a < 0));
  }
  // Ids unique and found again; positions on the plane; FP32 copies exact.
  std::set<Id> ids;
  for (std::uint32_t i = 0; i < seam.vertices.size(); ++i) {
    const auto& v = seam.vertices[i];
    CHECK(ids.insert(v.id).second);
    CHECK(seam.find(v.id) == i);
    CHECK(v.position == seam.frame.to_model(v.uv));
    CHECK(v.position32 == vs::core::to_fp32(v.position));
  }
  // Outline divisions no longer than the spacing (chords of arcs too).
  for (const auto& loop : seam.loops) {
    for (std::size_t i = 0; i < loop.vertices.size(); ++i) {
      const dvec2 a = seam.vertices[loop.vertices[i]].uv;
      const dvec2 b = seam.vertices[loop.vertices[(i + 1) % loop.vertices.size()]].uv;
      CHECK(vs::core::length(b - a) <= seam.spacing * (1 + 1e-9));
    }
  }
  // Each outline segment starts at its exact start point.
  for (std::size_t l = 0; l < seam.loops.size(); ++l) {
    const auto& loop = seam.loops[l];
    REQUIRE(loop.segment_starts.size() == seam.outline.loops[l].segments.size());
    for (std::size_t s = 0; s < loop.segment_starts.size(); ++s) {
      CHECK(seam.vertices[loop.vertices[loop.segment_starts[s]]].uv ==
            seam.outline.loops[l].segments[s].start);
    }
  }
  // The triangulation is a disc with holes: one shell, every outline edge
  // on the boundary once, χ = 1 − holes.
  vs::mesh::Mesh m;
  const auto map = vs::mesh::add_seam_vertices(m, seam);
  vs::mesh::add_seam_face(m, seam, map, seam.normal(), Id::derive(Id::null(), "v"));
  const auto r = vs::mesh::analyse(m);
  CHECK(r.manifold());
  CHECK(r.shells == 1);
  CHECK(r.boundary_edges.size() == seam.outline_vertex_count);
  CHECK(r.vertices == seam.vertices.size());
  CHECK(r.euler_characteristic() == 1 - static_cast<long long>(seam.loops.size() - 1));
  CHECK(r.zero_area_triangles == 0);
  double polygon = 0;
  for (std::size_t l = 0; l < seam.loops.size(); ++l) {
    polygon += vs::mesh::signed_double_area(uv, seam.loops[l].vertices) / 2;
  }
  CHECK_THAT(seam.area(), WithinRel(polygon, 1e-9));
}

}  // namespace

TEST_CASE("A rectangle's seam divides each side at the spacing", "[mesh][seam]") {
  const SeamOutline outline = meshers::rectangle({}, {0, 0}, 40, 20);
  auto seam = make_seam(surface(), outline, 5.0, kSettings);
  REQUIRE(seam.ok());
  check_seam(*seam);
  CHECK(seam->outline_vertex_count == 24);
  CHECK(seam->loops.size() == 1);
  CHECK(seam->loops[0].segment_starts == std::vector<std::uint32_t>{0, 8, 12, 20});
  CHECK(seam->vertices.size() > seam->outline_vertex_count);  // interior lattice points
  CHECK_THAT(seam->area(), WithinRel(800.0, 1e-12));
  CHECK(seam->surface == surface().id);
  CHECK(seam->name == "patch");
  // Interior points keep half a spacing from the outline.
  for (std::size_t i = seam->outline_vertex_count; i < seam->vertices.size(); ++i) {
    const dvec2 p = seam->vertices[i].uv;
    CHECK(std::min({p.x + 20, 20 - p.x, p.y + 10, 10 - p.y}) >= 2.5 - 1e-12);
  }
  // A spacing that doesn't divide the sides evenly rounds the count up.
  auto odd = make_seam(surface(), outline, 7.0, kSettings);
  REQUIRE(odd.ok());
  check_seam(*odd);
  CHECK(odd->outline_vertex_count == 6 + 3 + 6 + 3);
}

TEST_CASE("Seams are a pure function of their inputs", "[mesh][seam]") {
  std::mt19937_64 rng(7);
  const auto frame = meshers::random_frame(rng, 300);
  const SeamOutline outline = meshers::random_outline(rng, frame, 30, 3);
  auto a = make_seam(surface(), outline, 2.5, kSettings);
  auto b = make_seam(surface(), outline, 2.5, kSettings);
  REQUIRE(a.ok());
  REQUIRE(b.ok());
  CHECK(a->key == b->key);
  CHECK(a->triangles == b->triangles);
  REQUIRE(a->vertices.size() == b->vertices.size());
  for (std::size_t i = 0; i < a->vertices.size(); ++i) {
    CHECK(a->vertices[i].id == b->vertices[i].id);
    CHECK(a->vertices[i].position32 == b->vertices[i].position32);
  }
  // Another spacing or another surface: new ids.
  auto c = make_seam(surface(), outline, 3.0, kSettings);
  auto d = make_seam(surface("other"), outline, 2.5, kSettings);
  REQUIRE(c.ok());
  REQUIRE(d.ok());
  CHECK(c->key != a->key);
  CHECK(c->vertices[0].id != a->vertices[0].id);
  CHECK(d->vertices[0].id != a->vertices[0].id);
}

TEST_CASE("Loops are oriented whatever order they come in", "[mesh][seam]") {
  SeamOutline outline = meshers::rectangle({}, {0, 0}, 20, 20);
  // Outer loop clockwise, hole counter-clockwise: both the wrong way.
  OutlineLoop reversed_outer;
  for (auto it = outline.loops[0].segments.rbegin(); it != outline.loops[0].segments.rend(); ++it) {
    reversed_outer.segments.push_back(it->reversed());
  }
  outline.loops[0] = reversed_outer;
  outline.loops.push_back(OutlineLoop{{OutlineSegment::circle({0, 0}, 3)}});
  auto seam = make_seam(surface(), outline, 2.0, kSettings);
  REQUIRE(seam.ok());
  check_seam(*seam);
  CHECK(seam->outline.loops[1].segments[0].sweep < 0);
}

TEST_CASE("Arcs get at least sixteen pieces a turn", "[mesh][seam]") {
  // A 20 x 10 slot: two straight sides and two half circles.
  SeamOutline outline;
  outline.loops.push_back(OutlineLoop{{
      OutlineSegment::line({-5, -5}, {5, -5}),
      OutlineSegment::arc({5, 0}, 5, -kPi / 2, kPi),
      OutlineSegment::line({5, 5}, {-5, 5}),
      OutlineSegment::arc({-5, 0}, 5, kPi / 2, kPi),
  }});
  auto seam = make_seam(surface(), outline, 4.0, kSettings);
  REQUIRE(seam.ok());
  check_seam(*seam);
  // Lines: ceil(10 / 4) = 3; half circles: max(ceil(15.7 / 4), 8) = 8.
  CHECK(seam->loops[0].segment_starts == std::vector<std::uint32_t>{0, 3, 11, 14});
  // Arc vertices lie on their circles.
  for (std::uint32_t k = 3; k < 11; ++k) {
    const dvec2 p = seam->vertices[seam->loops[0].vertices[k]].uv;
    CHECK_THAT(vs::core::length(p - dvec2{5, 0}), WithinAbs(5.0, 1e-12));
  }
  // A coarse spacing still makes a usable hole.
  outline.loops.push_back(OutlineLoop{{OutlineSegment::circle({0, 0}, 1)}});
  auto coarse = make_seam(surface(), outline, 4.5, kSettings);
  REQUIRE(coarse.ok());
  check_seam(*coarse);
  CHECK(coarse->loops[1].vertices.size() == 16);
}

TEST_CASE("Bad spacings and outlines are refused", "[mesh][seam]") {
  const SeamOutline outline = meshers::rectangle({}, {0, 0}, 40, 20);
  for (const double s : {0.0, -1.0, std::nan(""), 1e-4}) {
    auto seam = make_seam(surface(), outline, s, kSettings);
    REQUIRE_FALSE(seam.ok());
    CHECK(seam.error().code == "mesh.seam.spacing");
    CHECK(seam.error().target()->id == surface().id);
  }
  auto fine = make_seam(surface(), meshers::rectangle({}, {0, 0}, 1000, 1000), 0.01, kSettings);
  REQUIRE_FALSE(fine.ok());
  CHECK(fine.error().code == "mesh.seam.too_fine");

  auto empty = make_seam(surface(), SeamOutline{}, 1.0, kSettings);
  REQUIRE_FALSE(empty.ok());
  CHECK(empty.error().code == "mesh.seam.outline");

  // A hole crossing the outline: the triangulator refuses, naming the surface.
  SeamOutline crossing = outline;
  crossing.loops.push_back(OutlineLoop{{OutlineSegment::circle({20, 0}, 3)}});
  auto bad = make_seam(surface(), crossing, 2.0, kSettings);
  REQUIRE_FALSE(bad.ok());
  CHECK(bad.error().code == "mesh.triangulate.self_intersecting");
  CHECK(bad.error().target()->kind == SubjectKind::ConnectionSurface);
}

TEST_CASE("Derived spacing is a sixteenth of the diagonal", "[mesh][seam]") {
  const SeamOutline outline = meshers::rectangle({}, {0, 0}, 30, 40);
  CHECK_THAT(vs::mesh::derive_spacing(outline, kSettings), WithinRel(50.0 / 16, 1e-12));
  CHECK(vs::mesh::derive_spacing(meshers::rectangle({}, {0, 0}, 1e-5, 1e-5), kSettings) ==
        kSettings.min_feature_size_mm());
}

TEST_CASE("Seam faces turn to face either side", "[mesh][seam]") {
  auto seam = make_seam(surface(), meshers::rectangle({}, {0, 0}, 10, 10), 2.0, kSettings);
  REQUIRE(seam.ok());
  const auto up = seam->triangles_facing({0, 0, 1});
  const auto down = seam->triangles_facing({0, 0, -1});
  REQUIRE(up.size() == down.size());
  CHECK(up == seam->triangles);
  for (std::size_t i = 0; i < up.size(); ++i) {
    CHECK(down[i][0] == up[i][0]);
    CHECK(down[i][1] == up[i][2]);
    CHECK(down[i][2] == up[i][1]);
  }
  vs::mesh::Mesh m;
  const auto map = vs::mesh::add_seam_vertices(m, *seam);
  CHECK(m.vertex_count() == seam->vertices.size());
  CHECK(m.seam_vertices[map[3]] == seam->vertices[3].id);
  CHECK(m.positions[map[3]] == seam->vertices[3].position32);
}

TEST_CASE("Random outlines, frames and spacings all make valid seams", "[mesh][seam][property]") {
  std::mt19937_64 rng(1119);
  for (int iteration = 0; iteration < 500; ++iteration) {
    CAPTURE(iteration);
    const double size = std::pow(10.0, std::uniform_real_distribution<double>(0.3, 2.3)(rng));
    const auto frame = meshers::random_frame(rng, 1000 - 2 * size);
    const SeamOutline outline = meshers::random_outline(rng, frame, size, 4);
    const double spacing = size * std::uniform_real_distribution<double>(0.02, 0.4)(rng);
    auto seam = make_seam(surface(), outline, spacing, kSettings);
    REQUIRE(seam.ok());
    check_seam(*seam);
  }
}

TEST_CASE("Seam timing for large patches", "[.perf]") {
  for (const double spacing : {2.0, 1.0, 0.5}) {
    const auto t0 = std::chrono::steady_clock::now();
    auto seam = make_seam(surface(), meshers::rectangle({}, {0, 0}, 200, 200), spacing, kSettings);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(seam.ok());
    WARN("spacing " << spacing << ": " << seam->vertices.size() << " vertices, "
                    << seam->triangles.size() << " triangles, " << ms << " ms");
  }
}

TEST_CASE("Seam timing for a long thin slot", "[.perf]") {
  for (const double spacing : {0.04, 0.02, 0.01}) {
    const auto t0 = std::chrono::steady_clock::now();
    auto seam = make_seam(surface(), meshers::rectangle({}, {0, 0}, 100, 0.05), spacing, kSettings);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(seam.ok());
    WARN("slot spacing " << spacing << ": " << seam->vertices.size() << " vertices, " << ms << " ms");
  }
}

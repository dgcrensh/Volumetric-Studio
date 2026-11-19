#include <cmath>
#include <random>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "meshers.hpp"
#include "vs/core/settings.hpp"
#include "vs/mesh/seam_library.hpp"
#include "vs/mesh/weld.hpp"

using Catch::Matchers::WithinRel;
using vs::core::dvec2;
using vs::core::dvec3;
using vs::core::Id;
using vs::core::SubjectKind;
using vs::core::SubjectRef;
using vs::mesh::Mesh;
using vs::mesh::Seam;
using vs::mesh::SeamLibrary;
using vs::mesh::SeamOutline;
using vs::mesh::weld;

namespace {

const auto kSettings = vs::core::ProductSettings::defaults();
const Id kA = Id::derive(Id::null(), "A");
const Id kB = Id::derive(Id::null(), "B");
const Id kC = Id::derive(Id::null(), "C");

Seam seam_for(const char* name, const SeamOutline& outline, double spacing,
              std::vector<Id> volumes) {
  const SubjectRef s{SubjectKind::ConnectionSurface, Id::derive(Id::null(), name), name};
  auto seam = vs::mesh::make_seam(s, outline, spacing, kSettings);
  REQUIRE(seam.ok());
  seam->volumes = std::move(volumes);
  return std::move(seam).value();
}

SeamLibrary library(std::vector<Seam> seams) {
  SeamLibrary lib;
  lib.volume_names = {{kA, "A"}, {kB, "B"}, {kC, "C"}};
  for (Seam& s : seams) lib.add(std::move(s));
  return lib;
}

SeamOutline translated(const SeamOutline& o, const dvec3& by) {
  SeamOutline out = o;
  out.frame.origin = o.frame.origin + by;
  return out;
}

// The welded result must be one closed, manifold shell with no errors.
void check_welded(const vs::mesh::WeldResult& r, std::size_t holes) {
  INFO([&] {
    std::string s;
    for (const auto& d : r.diagnostics) s += d.to_string() + "\n";
    return s;
  }());
  CHECK(r.ok());
  CHECK(r.topology.watertight());
  CHECK(r.topology.manifold());
  CHECK(r.topology.shells == 1);
  CHECK(r.topology.euler_characteristic() == 2 - 2 * static_cast<long long>(holes));
}

std::string codes(const vs::core::Diagnostics& ds) {
  std::string s;
  for (const auto& d : ds) s += (s.empty() ? "" : " ") + d.code;
  return s;
}

}  // namespace

TEST_CASE("Two prisms meshed independently weld on every random seam",
          "[mesh][weld][property]") {
  std::mt19937_64 rng(42);
  std::uniform_real_distribution<double> u(0, 1);
  for (int iteration = 0; iteration < 500; ++iteration) {
    CAPTURE(iteration);
    const double size = std::pow(10.0, 0.3 + 2 * u(rng));
    const auto frame = meshers::random_frame(rng, 1000 - 4 * size);
    const SeamOutline outline = meshers::random_outline(rng, frame, size, 3);
    const double spacing = size * (0.03 + 0.3 * u(rng));
    const Seam seam = seam_for("S", outline, spacing, {kA, kB});
    const std::size_t holes = seam.loops.size() - 1;
    const dvec3 n = seam.normal();
    // A below, B above, each with its own far cap spacing and height.
    const double ha = size * (0.1 + u(rng)), hb = size * (0.1 + u(rng));
    const SeamOutline below = translated(seam.outline, n * -ha);
    const SeamOutline above = translated(seam.outline, n * hb);
    const Mesh a = meshers::prism({nullptr, &below, size * (0.02 + 0.5 * u(rng))}, {&seam}, kA);
    const Mesh b = meshers::prism({&seam}, {nullptr, &above, size * (0.02 + 0.5 * u(rng))}, kB);
    const SeamLibrary lib = library({seam});
    const Mesh both[] = {a, b};
    const auto r = weld(both, lib);
    check_welded(r, holes);
    CHECK(r.internal_triangles == 2 * seam.triangles.size());
    CHECK(r.merged_vertices == seam.vertices.size());
    // Volume: about the patch's area times the total height (the far caps
    // divide hole arcs more coarsely than the seam).
    CHECK_THAT(vs::mesh::enclosed_volume(r.mesh), WithinRel(seam.area() * (ha + hb), 2e-2));
    // Each triangle keeps its volume.
    CHECK(r.mesh.volumes().size() == 2);
  }
}

TEST_CASE("Different meshers on each side weld too", "[mesh][weld][property]") {
  std::mt19937_64 rng(4242);
  std::uniform_real_distribution<double> u(0, 1);
  for (int iteration = 0; iteration < 200; ++iteration) {
    CAPTURE(iteration);
    const double size = std::pow(10.0, 0.5 + 1.5 * u(rng));
    const auto frame = meshers::random_frame(rng, 500);
    const SeamOutline outline = meshers::random_outline(rng, frame, size, 0);
    const Seam seam = seam_for("S", outline, size * (0.05 + 0.3 * u(rng)), {kA, kB});
    SECTION("a pyramid on a box, the seam smaller than the box's face") {
      const Mesh a = meshers::box_under(seam, size * (0.05 + u(rng)), size * (0.1 + u(rng)), kA);
      const Mesh b = meshers::pyramid(seam, size * (0.1 + u(rng)), +1, kB);
      const Mesh both[] = {b, a};
      check_welded(weld(both, library({seam})), 0);
    }
    SECTION("pyramids on both sides") {
      const Mesh a = meshers::pyramid(seam, size * (0.1 + u(rng)), -1, kA);
      const Mesh b = meshers::pyramid(seam, size * (0.1 + u(rng)), +1, kB);
      const Mesh both[] = {a, b};
      const auto r = weld(both, library({seam}));
      check_welded(r, 0);
      // Only the outline's vertices and the two apexes are left.
      CHECK(r.mesh.vertex_count() == seam.outline_vertex_count + 2);
    }
  }
}

TEST_CASE("A chain of three volumes across two seams welds", "[mesh][weld][property]") {
  std::mt19937_64 rng(777);
  std::uniform_real_distribution<double> u(0, 1);
  for (int iteration = 0; iteration < 150; ++iteration) {
    CAPTURE(iteration);
    const double size = std::pow(10.0, 0.5 + 1.5 * u(rng));
    const auto frame = meshers::random_frame(rng, 500);
    const SeamOutline outline = meshers::random_outline(rng, frame, size, 2);
    const dvec3 n = frame.normal();
    const double h = size * (0.1 + u(rng));
    // S1 at the frame, S2 h above it, each with its own spacing; B between.
    const Seam s1 = seam_for("S1", outline, size * (0.04 + 0.3 * u(rng)), {kA, kB});
    const Seam s2 = seam_for("S2", translated(s1.outline, n * h), size * (0.04 + 0.3 * u(rng)),
                             {kB, kC});
    const SeamOutline bottom = translated(s1.outline, n * -size);
    const SeamOutline top = translated(s2.outline, n * size);
    const Mesh a = meshers::prism({nullptr, &bottom, size * 0.2}, {&s1}, kA);
    const Mesh b = meshers::prism({&s1}, {&s2}, kB);
    const Mesh c = meshers::prism({&s2}, {nullptr, &top, size * 0.15}, kC);
    const Mesh all[] = {c, a, b};
    const auto r = weld(all, library({s1, s2}));
    check_welded(r, s1.loops.size() - 1);
    CHECK(r.mesh.volumes().size() == 3);
  }
}

TEST_CASE("A one-sided seam keeps its face", "[mesh][weld]") {
  const SeamOutline outline = meshers::rectangle({}, {0, 0}, 30, 20);
  const Seam seam = seam_for("S", outline, 3.0, {kA});
  const SeamOutline below = translated(seam.outline, {0, 0, -5});
  const Mesh a = meshers::prism({nullptr, &below, 4.0}, {&seam}, kA);
  const Mesh only[] = {a};
  const auto r = weld(only, library({seam}));
  check_welded(r, 0);
  CHECK(r.internal_triangles == 0);
  CHECK_THAT(vs::mesh::enclosed_volume(r.mesh), WithinRel(3000.0, 1e-6));
}

TEST_CASE("Non-conforming meshes fail, naming the connection surface", "[mesh][weld]") {
  std::mt19937_64 rng(5);
  const auto frame = meshers::random_frame(rng, 100);
  const SeamOutline outline = meshers::random_outline(rng, frame, 20, 1);
  const Seam seam = seam_for("Flange", outline, 2.0, {kA, kB});
  const dvec3 n = seam.normal();
  const SeamOutline below = translated(seam.outline, n * -10.0);
  const SeamOutline above = translated(seam.outline, n * 10.0);
  const Mesh a = meshers::prism({nullptr, &below, 3.0}, {&seam}, kA);
  const SeamLibrary lib = library({seam});

  const auto expect_surface_first = [&](const vs::mesh::WeldResult& r, const std::string& code) {
    INFO(codes(r.diagnostics));
    CHECK_FALSE(r.ok());
    const auto found = r.diagnostics.with_code(code);
    REQUIRE_FALSE(found.empty());
    REQUIRE(found.front()->target() != nullptr);
    CHECK(found.front()->target()->kind == SubjectKind::ConnectionSurface);
    CHECK(found.front()->target()->id == seam.surface);
    CHECK(found.front()->target()->label == "Flange");
  };

  SECTION("the other side ignores the seam and meshes at its own spacing") {
    // B's bottom is the same outline, but its own division, without ids.
    const Mesh b = meshers::prism({nullptr, &seam.outline, 2.7}, {nullptr, &above, 3.0}, kB);
    const Mesh both[] = {a, b};
    const auto r = weld(both, lib);
    expect_surface_first(r, "mesh.weld.nonconforming");
    // Structurally, the two closed prisms just sit side by side.
    CHECK(r.topology.shells == 2);
    CHECK(r.internal_triangles == 0);
  }
  SECTION("the other side uses the seam's outline but triangulates the inside itself") {
    Mesh b = meshers::prism({&seam}, {nullptr, &above, 3.0}, kB);
    // Replace B's seam face by an ear-free triangulation of the outline alone.
    Mesh rebuilt;
    const auto map = vs::mesh::add_seam_vertices(rebuilt, seam);
    std::vector<dvec2> uv;
    for (std::size_t i = 0; i < seam.outline_vertex_count; ++i) uv.push_back(seam.vertices[i].uv);
    std::vector<vs::mesh::Loop> loops;
    for (const auto& l : seam.loops) loops.push_back(l.vertices);
    auto own = vs::mesh::triangulate(uv, loops);
    REQUIRE(own.ok());
    for (const auto& t : *own) rebuilt.add_triangle(map[t[0]], map[t[2]], map[t[1]], kB);
    // Keep B's other triangles (those not entirely on seam vertices).
    for (std::size_t t = 0; t < b.triangle_count(); ++t) {
      const auto tri = b.triangle(t);
      if (!b.seam_vertices[tri[0]].is_null() && !b.seam_vertices[tri[1]].is_null() &&
          !b.seam_vertices[tri[2]].is_null()) {
        continue;
      }
      const auto base = static_cast<std::uint32_t>(rebuilt.vertex_count());
      for (const auto v : tri) rebuilt.add_vertex(b.positions[v], b.seam_vertices[v]);
      rebuilt.add_triangle(base, base + 1, base + 2, kB);
    }
    const Mesh both[] = {a, rebuilt};
    expect_surface_first(weld(both, lib), "mesh.weld.nonconforming");
  }
  SECTION("a seam vertex is moved") {
    Mesh b = meshers::prism({&seam}, {nullptr, &above, 3.0}, kB);
    b.positions[5].x += 0.01f;
    const Mesh both[] = {a, b};
    const auto r = weld(both, lib);
    expect_surface_first(r, "mesh.weld.moved");
    CHECK(r.diagnostics.with_code("mesh.weld.moved").front()->subjects[1].id == kB);
  }
  SECTION("a vertex carries an unknown id") {
    Mesh b = meshers::prism({&seam}, {nullptr, &above, 3.0}, kB);
    b.seam_vertices[b.vertex_count() - 1] = Id::derive(Id::null(), "stray");
    const Mesh both[] = {a, b};
    const auto r = weld(both, lib);
    REQUIRE(r.diagnostics.with_code("mesh.weld.unknown_vertex").size() == 1);
    CHECK(r.diagnostics.with_code("mesh.weld.unknown_vertex").front()->target()->id == kB);
  }
  SECTION("a third volume meshes a seam that doesn't join it") {
    const Mesh b = meshers::prism({&seam}, {nullptr, &above, 3.0}, kB);
    const Mesh c = meshers::pyramid(seam, 4.0, +1, kC);
    const Mesh all[] = {a, b, c};
    const auto r = weld(all, lib);
    expect_surface_first(r, "mesh.weld.not_joined");
  }
  SECTION("one side faces the wrong way") {
    Mesh b = meshers::prism({&seam}, {nullptr, &above, 3.0}, kB);
    // Flip B's seam triangles only.
    for (std::size_t t = 0; t < b.triangle_count(); ++t) {
      const auto tri = b.triangle(t);
      if (!b.seam_vertices[tri[0]].is_null() && !b.seam_vertices[tri[1]].is_null() &&
          !b.seam_vertices[tri[2]].is_null()) {
        std::swap(b.indices[3 * t + 1], b.indices[3 * t + 2]);
      }
    }
    const Mesh both[] = {a, b};
    const auto r = weld(both, lib);
    CHECK_FALSE(r.ok());
    CHECK_FALSE(r.topology.manifold());
  }
  SECTION("a broken mesh is reported before anything else") {
    Mesh b = meshers::prism({&seam}, {nullptr, &above, 3.0}, kB);
    b.indices[0] = 1u << 30;
    const Mesh both[] = {a, b};
    const auto r = weld(both, lib);
    CHECK(codes(r.diagnostics) == "mesh.invalid");
  }
}

TEST_CASE("Awkward outlines weld: thin slots, combs, far from the origin", "[mesh][weld]") {
  vs::sketch::SketchPlane far;
  far.origin = {990, -990, 995};
  SeamOutline slot = meshers::rectangle(far, {0, 0}, 100, 0.05);
  // A comb: ten 0.5 mm teeth 8 mm deep with 0.2 mm gaps.
  SeamOutline comb;
  comb.frame = {{-300, 200, 7}, {0, 0, 1}, {1, 0, 0}};
  {
    std::vector<dvec2> p{{0, 0}, {7, 0}};
    for (int k = 0; k < 10; ++k) {
      const double x = 7.0 - k * 0.7;
      p.push_back({x, 8});
      p.push_back({x - 0.5, 8});
      p.push_back({x - 0.5, 1});
      p.push_back({x - 0.7, 1});
    }
    p.back() = {0, 1};
    vs::mesh::OutlineLoop loop;
    for (std::size_t k = 0; k < p.size(); ++k) {
      loop.segments.push_back(vs::mesh::OutlineSegment::line(p[k], p[(k + 1) % p.size()]));
    }
    comb.loops.push_back(loop);
  }
  for (const auto& [outline, spacing] :
       std::vector<std::pair<SeamOutline, double>>{{slot, 1.0}, {slot, 0.04}, {comb, 0.05}, {comb, 3.0}}) {
    CAPTURE(spacing);
    const Seam seam = seam_for("S", outline, spacing, {kA, kB});
    const dvec3 n = seam.normal();
    const SeamOutline below = translated(seam.outline, n * -2.0);
    const SeamOutline above = translated(seam.outline, n * 3.0);
    const Mesh a = meshers::prism({nullptr, &below, 0.37}, {&seam}, kA);
    const Mesh b = meshers::prism({&seam}, {nullptr, &above, 5.0}, kB);
    const Mesh both[] = {a, b};
    check_welded(weld(both, library({seam})), 0);
  }
}

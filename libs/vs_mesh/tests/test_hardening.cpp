// H4 (plan docs/plans/H-hardening.md, decision 0026): regression tests for
// the mesh geometry, CDT, weld and mesh file fixes. Each one failed on
// main before its fix.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <utility>

#if defined(__linux__)
#include <sys/resource.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <miniz.h>

#include "vs/core/settings.hpp"
#include "vs/mesh/io.hpp"
#include "vs/mesh/seam_library.hpp"
#include "vs/mesh/weld.hpp"
#include "vs/mesh/seam.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/mesh/triangulate.hpp"

using Catch::Matchers::WithinRel;
using vs::core::dvec2;
using vs::core::fvec3;
using vs::core::Id;
using vs::mesh::Bytes;
using vs::mesh::Mesh;

namespace {

const auto kSettings = vs::core::ProductSettings::defaults();

Bytes bytes_of(const std::string& text) {
  Bytes out;
  for (const char c : text) out.push_back(static_cast<std::byte>(c));
  return out;
}

std::string code_of(const vs::core::Result<Mesh>& r) { return r ? std::string() : r.error().code; }

// A 3MF package holding `model` as its model part (stored, no relationships).
Bytes package(const std::string& model) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof zip);
  REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
  REQUIRE(mz_zip_writer_add_mem(&zip, "3D/3dmodel.model", model.data(), model.size(),
                                static_cast<mz_uint>(MZ_NO_COMPRESSION)));
  void* data = nullptr;
  std::size_t size = 0;
  REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &data, &size));
  const auto* first = static_cast<const std::byte*>(data);
  Bytes out(first, first + size);
  mz_free(data);
  mz_zip_writer_end(&zip);
  return out;
}

// A cube of side `s` at `lo`, each face split into n x n squares.
Mesh cube(const vs::core::dvec3& lo, double s, int n) {
  Mesh m;
  std::map<std::array<int, 3>, std::uint32_t> index;
  const auto vertex = [&](int i, int j, int k) {
    const auto [it, added] = index.try_emplace({i, j, k}, 0);
    if (added) {
      it->second = m.add_vertex(vs::core::to_fp32(
          vs::core::dvec3{lo.x + s * i / n, lo.y + s * j / n, lo.z + s * k / n}));
    }
    return it->second;
  };
  // Each face: axis a fixed at 0 or n, outward along -a or +a.
  for (int a = 0; a < 3; ++a) {
    for (int side = 0; side < 2; ++side) {
      const int b = (a + 1) % 3, c = (a + 2) % 3;
      for (int u = 0; u < n; ++u) {
        for (int v = 0; v < n; ++v) {
          const auto at = [&](int du, int dv) {
            std::array<int, 3> p{};
            p[static_cast<std::size_t>(a)] = side * n;
            p[static_cast<std::size_t>(b)] = u + du;
            p[static_cast<std::size_t>(c)] = v + dv;
            return vertex(p[0], p[1], p[2]);
          };
          const std::uint32_t p00 = at(0, 0), p10 = at(1, 0), p11 = at(1, 1), p01 = at(0, 1);
          if (side == 1) {
            m.add_triangle(p00, p10, p11, Id::null());
            m.add_triangle(p00, p11, p01, Id::null());
          } else {
            m.add_triangle(p00, p11, p10, Id::null());
            m.add_triangle(p00, p01, p11, Id::null());
          }
        }
      }
    }
  }
  return m;
}

}  // namespace

// --- mesh invariants -------------------------------------------------------------

TEST_CASE("Mesh::check() refuses non-finite positions", "[mesh][hardening]") {
  Mesh m;
  m.add_vertex({0, 0, 0});
  m.add_vertex({1, 0, 0});
  m.add_vertex({0, std::numeric_limits<float>::quiet_NaN(), 0});
  m.add_triangle(0, 1, 2, Id::null());
  const auto ds = m.check();
  REQUIRE(ds.has_errors());
  CHECK(ds.begin()->code == "mesh.invalid");
  m.positions[2] = {0, std::numeric_limits<float>::infinity(), 0};
  CHECK(m.check().has_errors());
  m.positions[2] = {0, 1, 0};
  CHECK_FALSE(m.check().has_errors());
}

TEST_CASE("A mesh appended to itself doubles", "[mesh][hardening]") {
  Mesh m = cube({0, 0, 0}, 1, 1);
  const Mesh original = m;
  m.append(m);
  CHECK(m.vertex_count() == 2 * original.vertex_count());
  CHECK(m.triangle_count() == 2 * original.triangle_count());
  for (std::size_t t = 0; t < original.triangle_count(); ++t) {
    const auto a = original.triangle(t);
    const auto b = m.triangle(t + original.triangle_count());
    for (std::size_t k = 0; k < 3; ++k) {
      CHECK(b[k] == a[k] + original.vertex_count());
      CHECK(m.positions[b[k]] == original.positions[a[k]]);
    }
  }
  CHECK_FALSE(m.check().has_errors());
}

TEST_CASE("Enclosed volume keeps its precision far from the origin", "[mesh][hardening]") {
  // A 0.1 mm cube near the working space's corner: summing tetrahedra to
  // the origin loses most of its digits; to a vertex of the mesh, none.
  // The faces stay planar in FP32, so the exact volume is the product of
  // the rounded sides.
  const vs::core::dvec3 lo{987.1, -993.3, 999.7};
  const double s = 0.1;
  const auto side = [&](double a) {
    return static_cast<double>(static_cast<float>(a + s)) - static_cast<double>(static_cast<float>(a));
  };
  const double exact = side(lo.x) * side(lo.y) * side(lo.z);
  const Mesh far = cube(lo, s, 7);
  CHECK_THAT(vs::mesh::enclosed_volume(far), WithinRel(exact, 1e-9));
}

// --- mesh files -----------------------------------------------------------------

TEST_CASE("OBJ: out-of-range negative indices and inline comments", "[mesh][io][obj][hardening]") {
  const std::string verts = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
  // -LLONG_MIN overflows; it must be refused, not read out of bounds.
  CHECK(code_of(vs::mesh::read_obj(bytes_of(verts + "f 1 2 -9223372036854775808\n"))) ==
        "mesh.io.obj");
  CHECK(code_of(vs::mesh::read_obj(bytes_of(verts + "f 1 2 -4\n"))) == "mesh.io.obj");
  CHECK(code_of(vs::mesh::read_obj(bytes_of(verts + "f 1 2 9223372036854775807\n"))) ==
        "mesh.io.obj");
  CHECK(vs::mesh::read_obj(bytes_of(verts + "f -3 -2 -1\n")).ok());

  // A comment may end any line.
  const auto m = vs::mesh::read_obj(
      bytes_of("v 0 0 0 # origin\nv 1 0 0#x\nv 0 1 0\nf 1 2 3 # the only face\n# done\n"));
  REQUIRE(m.ok());
  CHECK(m->triangle_count() == 1);
  CHECK(m->positions[1] == fvec3{1, 0, 0});
}

TEST_CASE("Coordinates that overflow a float are refused", "[mesh][io][hardening]") {
  CHECK(code_of(vs::mesh::read_obj(bytes_of("v 1e39 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"))) ==
        "mesh.io.obj");
  CHECK(code_of(vs::mesh::read_stl(bytes_of("solid x\n facet normal 0 0 1\n  outer loop\n"
                                            "   vertex 0 0 0\n   vertex 1 0 0\n"
                                            "   vertex 0 -4e38 0\n  endloop\n endfacet\n"
                                            "endsolid x\n"))) == "mesh.io.stl");
  const std::string model =
      "<?xml version=\"1.0\"?>\n<model unit=\"meter\" "
      "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\"><resources>"
      "<object id=\"1\" type=\"model\"><mesh><vertices>"
      "<vertex x=\"0\" y=\"0\" z=\"0\"/><vertex x=\"1e36\" y=\"0\" z=\"0\"/>"
      "<vertex x=\"0\" y=\"1\" z=\"0\"/></vertices><triangles>"
      "<triangle v1=\"0\" v2=\"1\" v3=\"2\"/></triangles></mesh></object></resources>"
      "<build><item objectid=\"1\"/></build></model>\n";
  CHECK(code_of(vs::mesh::read_3mf(package(model))) == "mesh.io.3mf");
}

TEST_CASE("Writers refuse a broken mesh", "[mesh][io][hardening]") {
  Mesh m;
  m.add_vertex({0, 0, 0});
  m.add_vertex({1, 0, 0});
  m.add_vertex({0, 1, 0});
  m.add_triangle(0, 1, 7, Id::null());  // out of range
  const auto stl = vs::mesh::write_stl(m);
  REQUIRE_FALSE(stl.ok());
  CHECK(stl.error().code == "mesh.invalid");
  const vs::mesh::ThreeMfObject object{"Broken", Id::null(), &m};
  const auto three_mf = vs::mesh::write_3mf(std::span(&object, 1), "Broken");
  REQUIRE_FALSE(three_mf.ok());
  CHECK(three_mf.error().code == "mesh.invalid");
  m.positions[2] = {0, std::numeric_limits<float>::infinity(), 0};
  m.indices[2] = 2;
  CHECK(vs::mesh::write_stl(m).error().code == "mesh.invalid");
}

TEST_CASE("3MF: elements with end tags, namespace prefixes and character references",
          "[mesh][io][3mf][hardening]") {
  SECTION("vertices, triangles, components and items written with end tags") {
    const std::string model =
        "<?xml version=\"1.0\"?>\n<model unit=\"millimeter\" "
        "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\"><resources>"
        "<object id=\"1\" type=\"model\"><mesh><vertices>"
        "<vertex x=\"0\" y=\"0\" z=\"0\"></vertex><vertex x=\"1\" y=\"0\" z=\"0\"></vertex>"
        "<vertex x=\"0\" y=\"1\" z=\"0\"></vertex><vertex x=\"0\" y=\"0\" z=\"1\"></vertex>"
        "</vertices><triangles>"
        "<triangle v1=\"0\" v2=\"2\" v3=\"1\"></triangle><triangle v1=\"0\" v2=\"1\" v3=\"3\"></triangle>"
        "<triangle v1=\"0\" v2=\"3\" v3=\"2\"></triangle><triangle v1=\"1\" v2=\"2\" v3=\"3\"></triangle>"
        "</triangles></mesh></object>"
        "<object id=\"2\" type=\"model\"><components>"
        "<component objectid=\"1\" transform=\"1 0 0 0 1 0 0 0 1 10 0 0\"></component>"
        "</components></object></resources>"
        "<build><item objectid=\"2\"></item></build></model>\n";
    const auto m = vs::mesh::read_3mf(package(model));
    INFO((m ? std::string() : m.error().to_string()));
    REQUIRE(m.ok());
    CHECK(m->triangle_count() == 4);
    CHECK(m->bounds().min == fvec3{10, 0, 0});
    CHECK_THAT(vs::mesh::enclosed_volume(*m), WithinRel(1.0 / 6.0, 1e-6));
  }
  SECTION("every element with a namespace prefix") {
    const std::string model =
        "<?xml version=\"1.0\"?>\n<m:model unit=\"centimeter\" "
        "xmlns:m=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\"><m:resources>"
        "<m:object id=\"7\" type=\"model\"><m:mesh><m:vertices>"
        "<m:vertex x=\"0\" y=\"0\" z=\"0\"/><m:vertex x=\"1\" y=\"0\" z=\"0\"/>"
        "<m:vertex x=\"0\" y=\"1\" z=\"0\"/><m:vertex x=\"0\" y=\"0\" z=\"1\"/>"
        "</m:vertices><m:triangles>"
        "<m:triangle v1=\"0\" v2=\"2\" v3=\"1\"/><m:triangle v1=\"0\" v2=\"1\" v3=\"3\"/>"
        "<m:triangle v1=\"0\" v2=\"3\" v3=\"2\"/><m:triangle v1=\"1\" v2=\"2\" v3=\"3\"/>"
        "</m:triangles></m:mesh></m:object></m:resources>"
        "<m:build><m:item objectid=\"7\"/></m:build></m:model>\n";
    const auto m = vs::mesh::read_3mf(package(model));
    INFO((m ? std::string() : m.error().to_string()));
    REQUIRE(m.ok());
    CHECK(m->triangle_count() == 4);
    CHECK(m->bounds().max == fvec3{10, 10, 10});  // centimetres
  }
  SECTION("numeric character references in attribute values") {
    const std::string model =
        "<?xml version=\"1.0\"?>\n<model unit=\"millimeter\" "
        "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\"><resources>"
        "<object id=\"&#49;\" type=\"model\"><mesh><vertices>"
        "<vertex x=\"0\" y=\"0\" z=\"0\"/><vertex x=\"&#50;&#x35;\" y=\"0\" z=\"0\"/>"
        "<vertex x=\"0\" y=\"1\" z=\"0\"/></vertices><triangles>"
        "<triangle v1=\"0\" v2=\"1\" v3=\"2\"/></triangles></mesh></object></resources>"
        "<build><item objectid=\"1\"/></build></model>\n";
    const auto m = vs::mesh::read_3mf(package(model));
    INFO((m ? std::string() : m.error().to_string()));
    REQUIRE(m.ok());
    CHECK(m->bounds().max.x == 25.0f);
  }
}

TEST_CASE("Signed zeros are one position", "[mesh][io][seam][hardening]") {
  // Two triangles sharing the edge (0,0,0)-(1,0,0), one written with -0.
  const std::string stl =
      "solid z\n facet normal 0 0 1\n  outer loop\n   vertex 0 0 0\n   vertex 1 0 0\n"
      "   vertex 0 1 0\n  endloop\n endfacet\n facet normal 0 0 -1\n  outer loop\n"
      "   vertex 1 -0 -0\n   vertex -0 -0 0\n   vertex 0 -1 0\n  endloop\n endfacet\nendsolid z\n";
  const auto m = vs::mesh::read_stl(bytes_of(stl));
  REQUIRE(m.ok());
  CHECK(m->vertex_count() == 4);
  for (const fvec3& p : m->positions) {
    for (const float c : {p.x, p.y, p.z}) CHECK_FALSE((c == 0.0f && std::signbit(c)));
  }

  // A baked mesh that wrote -0 where the seam has +0 still finds the seam.
  const vs::core::SubjectRef surface{vs::core::SubjectKind::ConnectionSurface,
                                     Id::derive(Id::null(), "Zero"), "Zero"};
  vs::mesh::SeamOutline outline{
      vs::sketch::SketchPlane{},
      {vs::mesh::OutlineLoop{{vs::mesh::OutlineSegment::line({0, 0}, {4, 0}),
                              vs::mesh::OutlineSegment::line({4, 0}, {4, 4}),
                              vs::mesh::OutlineSegment::line({4, 4}, {0, 4}),
                              vs::mesh::OutlineSegment::line({0, 4}, {0, 0})}}}};
  const auto seam = vs::mesh::make_seam(surface, outline, 1.0, kSettings);
  REQUIRE(seam.ok());
  Mesh baked;
  std::size_t zeros = 0;
  for (const auto& v : seam->vertices) {
    fvec3 p = v.position32;
    for (float* c : {&p.x, &p.y, &p.z}) {
      if (*c == 0.0f) {
        *c = -0.0f;
        ++zeros;
      }
    }
    baked.add_vertex(p);
  }
  REQUIRE(zeros > 0);
  const vs::mesh::Seam* seams[] = {&*seam};
  CHECK(vs::mesh::attach_seam_vertices(baked, seams) == seam->vertices.size());
}

// --- the CDT ---------------------------------------------------------------------

namespace {

// The process's peak resident memory, KB (Linux; 0 elsewhere).
long peak_kb() {
#if defined(__linux__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) return usage.ru_maxrss;
#endif
  return 0;
}

// Area covered by the triangles, against the loops' own.
void check_cover(const std::vector<dvec2>& pts, const std::vector<vs::mesh::Loop>& loops,
                 const std::vector<vs::mesh::Triangle>& tris) {
  double area = 0;
  for (const auto& t : tris) {
    const dvec2 a = pts[t[0]], b = pts[t[1]], c = pts[t[2]];
    const double o = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    CHECK(o > 0);
    area += o / 2;
  }
  double expected = 0;
  for (std::size_t l = 0; l < loops.size(); ++l) {
    expected += std::abs(vs::mesh::signed_double_area(pts, loops[l])) / 2 * (l == 0 ? 1 : -1);
  }
  CHECK_THAT(area, WithinRel(expected, 1e-9));
}

}  // namespace

TEST_CASE("Mixed-scale outlines triangulate without a dense grid", "[mesh][triangulate][hardening]") {
  // A right triangle whose legs are divided 25,000 times each and whose
  // hypotenuse is one segment: a grid sized by the mean segment length
  // put the hypotenuse in tens of millions of cells.
  constexpr std::uint32_t n = 25000;
  std::vector<dvec2> pts;
  vs::mesh::Loop loop;
  for (std::uint32_t k = 0; k < n; ++k) {
    loop.push_back(static_cast<std::uint32_t>(pts.size()));
    pts.push_back({-500.0 + 1000.0 * k / n, -500.0});
  }
  for (std::uint32_t k = 0; k < n; ++k) {
    loop.push_back(static_cast<std::uint32_t>(pts.size()));
    pts.push_back({500.0, -500.0 + 1000.0 * k / n});
  }
  loop.push_back(static_cast<std::uint32_t>(pts.size()));
  pts.push_back({500.0, 500.0});
  const long peak_before = peak_kb();
  const auto start = std::chrono::steady_clock::now();
  const auto tris = vs::mesh::triangulate(pts, {loop});
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  // main's grid peaked at about 5 GB here; the tree takes a few MB.
  CHECK(peak_kb() - peak_before < 512 * 1024);
  REQUIRE(tris.ok());
  CHECK(tris->size() == pts.size() - 2);
  check_cover(pts, {loop}, *tris);
  CHECK(seconds < 20.0);

  // And a crossing far along the long side is still found.
  std::vector<dvec2> bent = pts;
  bent[n + n / 2] = {-400.0, -400.0 + 1000.0 * (n / 2) / n};  // pokes through the hypotenuse
  CHECK(vs::mesh::triangulate(bent, {loop}).error().code == "mesh.triangulate.self_intersecting");
}

TEST_CASE("A finely divided strip far from the origin triangulates",
          "[mesh][triangulate][hardening]") {
  // Found by fuzzing: a 0.3 x 12.9 mm strip near (727, -793), its sides
  // divided and its inside filled with a triangular lattice as a seam's
  // are. orient() disagreed with itself in the last bit depending on
  // which corner it measured from, and inserting a point failed for these
  // spacings (13,690 points each).
  const dvec2 origin{0x1.6b8f96d82febbp+9, -0x1.8c41002c95998p+9};
  const double w = 0x1.3213291ad0800p-2, h = 0x1.9c740c28fa5c0p+3;
  for (const double spacing : {0x1.2a932c8ca3ff1p-6, 0x1.2a932c8ca46b2p-6, 0x1.2a932c8ca4fb4p-6,
                               0x1.2a932c8ca58b6p-6, 0x1.2a932c8ca729bp-6}) {
    std::vector<dvec2> pts;
    vs::mesh::Loop loop;
    const dvec2 corners[4] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
    for (int s = 0; s < 4; ++s) {
      const dvec2 a = corners[s], b = corners[(s + 1) % 4];
      const auto n = static_cast<std::size_t>(std::ceil(vs::core::length(b - a) / spacing));
      for (std::size_t k = 0; k < n; ++k) {
        loop.push_back(static_cast<std::uint32_t>(pts.size()));
        pts.push_back(origin + a + (b - a) * (static_cast<double>(k) / static_cast<double>(n)));
      }
    }
    const double row = spacing * std::sqrt(3.0) / 2;
    // Rows and columns accumulated as the fuzzer did, to the bit.
    for (double y = row; y < h - 0.5 * spacing; y += row) {
      const double offset = static_cast<long>(y / row) % 2 != 0 ? 0.5 * spacing : 0.0;
      for (double x = spacing + offset; x < w - 0.5 * spacing; x += spacing) {
        pts.push_back(origin + dvec2{x, y});
      }
    }
    INFO("spacing " << spacing << ", " << pts.size() << " points");
    const auto tris = vs::mesh::triangulate(pts, {loop});
    INFO((tris.ok() ? std::string() : tris.error().to_string()));
    REQUIRE(tris.ok());
    check_cover(pts, {loop}, *tris);
  }
}

TEST_CASE("Polygons at extreme scales recover their edges", "[mesh][triangulate][hardening]") {
  // A segment across a square of scattered points: recovering it flips
  // many edges, and whether each new edge still crosses it was judged by
  // products of areas, which underflow to zero at 1e-100 (and the super
  // triangle had a 1 nm floor, so tiny inputs failed before that).
  for (const double scale : {1e-100, 1e-6, 1.0, 1e60}) {
    CAPTURE(scale);
    std::vector<dvec2> pts;
    vs::mesh::Loop loop;
    for (int k = 0; k < 40; ++k) {
      const int side = k / 10, i = k % 10;
      const double t = i / 10.0;
      const dvec2 p = side == 0 ? dvec2{t, 0} : side == 1 ? dvec2{1, t} : side == 2 ? dvec2{1 - t, 1} : dvec2{0, 1 - t};
      loop.push_back(static_cast<std::uint32_t>(pts.size()));
      pts.push_back(p * scale);
    }
    // Steiner points off the segment, from a fixed sequence.
    std::uint64_t state = 12345;
    const auto next = [&] {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      return static_cast<double>(state >> 11) / 9007199254740992.0;
    };
    for (int k = 0; k < 300; ++k) {
      const dvec2 p{0.02 + 0.96 * next(), 0.02 + 0.96 * next()};
      const double off = p.y - 0.5;  // off the segment
      if (std::abs(off) < 0.01) continue;
      pts.push_back(p * scale);
    }
    // From (0, 0.5) to (1, 0.5): loop points 35 and 15.
    const vs::mesh::Segment cut{35, 15};
    const auto tris = vs::mesh::triangulate(pts, {loop}, std::span(&cut, 1));
    INFO((tris ? std::string() : tris.error().to_string()));
    REQUIRE(tris.ok());
    check_cover(pts, {loop}, *tris);
    bool found = false;
    for (const auto& t : *tris) {
      for (std::size_t k = 0; k < 3; ++k) {
        const auto x = t[k], y = t[(k + 1) % 3];
        found = found || (x == 35 && y == 15) || (x == 15 && y == 35);
      }
    }
    CHECK(found);
  }
}

// --- the weld ---------------------------------------------------------------------

namespace {

// A pyramid on a seam with a hole: the seam's triangles and its hole
// filled, joined to an apex `h` along the normal times `side`.
Mesh filled_pyramid(const vs::mesh::Seam& seam, double h, int side, const Id& volume) {
  Mesh m;
  const auto map = vs::mesh::add_seam_vertices(m, seam);
  const vs::core::dvec3 n = seam.normal();
  const vs::core::dvec3 outward = n * static_cast<double>(-side);
  vs::mesh::add_seam_face(m, seam, map, outward, volume);
  for (const auto& fill : seam.hole_fills) {
    for (vs::mesh::Triangle t : fill) {
      if (side > 0) std::swap(t[1], t[2]);
      m.add_triangle(map[t[0]], map[t[1]], map[t[2]], volume);
    }
  }
  vs::core::dvec3 centre{0, 0, 0};
  const auto& outer = seam.loops[0].vertices;
  for (const auto v : outer) centre = centre + seam.vertices[v].position;
  centre = centre * (1.0 / static_cast<double>(outer.size()));
  const std::uint32_t apex = m.add_vertex(vs::core::to_fp32(centre + n * (h * side)));
  for (std::size_t i = 0; i < outer.size(); ++i) {
    const std::uint32_t a = map[outer[i]], b = map[outer[(i + 1) % outer.size()]];
    if (side > 0) {
      m.add_triangle(a, b, apex, volume);
    } else {
      m.add_triangle(b, a, apex, volume);
    }
  }
  return m;
}

}  // namespace

TEST_CASE("Hole fills are part of a seam's conformity", "[mesh][weld][hardening]") {
  const Id a = Id::derive(Id::null(), "A"), b = Id::derive(Id::null(), "B");
  vs::mesh::SeamOutline outline{
      vs::sketch::SketchPlane{},
      {vs::mesh::OutlineLoop{{vs::mesh::OutlineSegment::line({-10, -10}, {10, -10}),
                              vs::mesh::OutlineSegment::line({10, -10}, {10, 10}),
                              vs::mesh::OutlineSegment::line({10, 10}, {-10, 10}),
                              vs::mesh::OutlineSegment::line({-10, 10}, {-10, -10})}},
       vs::mesh::OutlineLoop{{vs::mesh::OutlineSegment::circle({0, 0}, 3)}}}};
  const vs::core::SubjectRef surface{vs::core::SubjectKind::ConnectionSurface,
                                     Id::derive(Id::null(), "Flange"), "Flange"};
  auto made = vs::mesh::make_seam(surface, outline, 2.0, kSettings);
  REQUIRE(made.ok());
  made->volumes = {a, b};
  REQUIRE(made->hole_fills.size() == 1);
  vs::mesh::SeamLibrary lib;
  lib.volume_names = {{a, "A"}, {b, "B"}};
  lib.add(*made);
  const vs::mesh::Seam& seam = lib.seams.front();

  const Mesh below = filled_pyramid(seam, 8, -1, a);
  const Mesh above = filled_pyramid(seam, 8, +1, b);
  CHECK(vs::mesh::analyse(below).watertight());
  CHECK(vs::mesh::analyse(above).watertight());
  {
    const Mesh both[] = {below, above};
    const auto r = vs::mesh::weld(both, lib);
    CHECK(r.ok());
    CHECK(r.topology.watertight());
  }
  const auto nonconforming_on_b = [&](const vs::mesh::WeldResult& r) {
    const auto found = r.diagnostics.with_code("mesh.weld.nonconforming");
    REQUIRE(found.size() == 1);
    CHECK(found.front()->subjects[0].id == seam.surface);
    CHECK(found.front()->subjects[1].id == b);
    return found.front()->message;
  };
  SECTION("half a hole's fill") {
    Mesh partial = above;
    // Drop the fill's first half: those triangles come right after the seam's own.
    const std::size_t first = seam.triangles.size();
    const std::size_t drop = seam.hole_fills[0].size() / 2;
    partial.indices.erase(partial.indices.begin() + static_cast<std::ptrdiff_t>(3 * first),
                          partial.indices.begin() + static_cast<std::ptrdiff_t>(3 * (first + drop)));
    partial.triangle_volumes.erase(
        partial.triangle_volumes.begin() + static_cast<std::ptrdiff_t>(first),
        partial.triangle_volumes.begin() + static_cast<std::ptrdiff_t>(first + drop));
    const Mesh both[] = {below, partial};
    nonconforming_on_b(vs::mesh::weld(both, lib));
  }
  SECTION("a seam triangle twice") {
    Mesh twice = above;
    const auto t = twice.triangle(0);
    twice.add_triangle(t, b);
    const Mesh both[] = {below, twice};
    // The message said "0 of N seam triangles missing": it now counts the repeat.
    const std::string message = nonconforming_on_b(vs::mesh::weld(both, lib));
    CHECK(message.find("1 repeated") != std::string::npos);
  }
}

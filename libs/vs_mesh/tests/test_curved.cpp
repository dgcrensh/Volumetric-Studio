// L2: seams on cylinders, and B-rep volumes conforming to them.

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "curved.hpp"
#include "vs/core/settings.hpp"
#include "vs/kernel/kernel.hpp"
#include "vs/mesh/combine.hpp"
#include "vs/mesh/seam_library.hpp"
#include "vs/model/build.hpp"
#include "vs/mesh/tessellate.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/mesh/weld.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using vs::core::dbox3;
using vs::core::dvec2;
using vs::core::dvec3;
using vs::core::Id;
using vs::core::SubjectKind;
using vs::core::SubjectRef;
using vs::kernel::Solid;
using vs::mesh::Mesh;
using vs::mesh::OutlineLoop;
using vs::mesh::OutlineSegment;
using vs::mesh::Seam;
using vs::mesh::SeamLibrary;
using vs::mesh::SeamOutline;
using vs::mesh::Triangle;

namespace {

const auto kSettings = vs::core::ProductSettings::defaults();
constexpr double kPi = std::numbers::pi;
const SubjectRef kInner{SubjectKind::Volume, Id::derive(Id::null(), "Inner"), "Inner"};
const SubjectRef kOuter{SubjectKind::Volume, Id::derive(Id::null(), "Outer"), "Outer"};
const vs::kernel::TessellationParams kFine{0.01, 0.2};

std::string describe(const vs::core::Diagnostics& ds) {
  std::string s;
  for (const auto& d : ds) s += d.to_string() + "\n";
  return s;
}

std::string codes(const vs::core::Diagnostics& ds) {
  std::string s;
  for (const auto& d : ds) s += (s.empty() ? "" : " ") + d.code;
  return s;
}

Solid box(dvec3 lo, dvec3 hi) {
  return vs::kernel::make_box(dbox3::from_min_max(lo, hi), kSettings).value();
}

Solid cylinder(dvec3 base, double r, double h) {
  return vs::kernel::make_cylinder({base, {0, 0, 1}}, r, h, kSettings).value();
}

Solid minus(const Solid& a, const Solid& b) { return vs::kernel::cut(a, b).value(); }

// A cylinder outline round the frame's normal: angles [a0, a0 + sweep],
// heights [y0, y1]; a band when the sweep is a full turn.
SeamOutline cylinder_outline(const vs::sketch::SketchPlane& frame, double r, double a0,
                             double sweep, double y0, double y1) {
  SeamOutline o;
  o.frame = frame;
  o.radius = r;
  const double x0 = r * a0, x1 = r * (a0 + sweep);
  const dvec2 c00{x0, y0}, c10{x1, y0}, c11{x1, y1}, c01{x0, y1};
  if (sweep >= 2 * kPi) {
    o.band = true;
    o.loops = {OutlineLoop{{OutlineSegment::line(c00, c10)}},
               OutlineLoop{{OutlineSegment::line(c11, c01)}}};
  } else {
    o.loops = {OutlineLoop{{OutlineSegment::line(c00, c10), OutlineSegment::line(c10, c11),
                            OutlineSegment::line(c11, c01), OutlineSegment::line(c01, c00)}}};
  }
  return o;
}

Seam seam_for(const char* name, const SeamOutline& outline, double spacing = 0) {
  const SubjectRef s{SubjectKind::ConnectionSurface, Id::derive(Id::null(), name), name};
  auto seam = vs::mesh::make_seam(
      s, outline, spacing > 0 ? spacing : vs::mesh::derive_spacing(outline, kSettings), kSettings);
  INFO((seam.ok() ? std::string() : seam.error().to_string()));
  REQUIRE(seam.ok());
  seam->volumes = {kInner.id, kOuter.id};
  return std::move(seam).value();
}

SeamLibrary library(Seam seam) {
  SeamLibrary lib;
  lib.volume_names = {{kInner.id, "Inner"}, {kOuter.id, "Outer"}};
  lib.add(std::move(seam));
  return lib;
}

vs::mesh::VolumeMesh mesh_of(const Solid& solid, const SubjectRef& volume, const Seam& seam) {
  const Seam* seams[] = {&seam};
  vs::mesh::VolumeMesh m = vs::mesh::tessellate(solid, volume, seams, kSettings, {kFine});
  INFO(volume.label << ": " << describe(m.diagnostics));
  CHECK(m.ok());
  CHECK(m.seams == std::vector<Id>{seam.surface});
  const vs::mesh::TopologyReport t = vs::mesh::analyse(m.mesh);
  CHECK(t.watertight());
  CHECK(t.manifold());
  return m;
}

// Tessellates both volumes against the seam, welds them, and checks the
// result is one closed shell enclosing `volume` (to the deflection).
void check_pair(const Solid& inner, const Solid& outer, const SeamLibrary& lib, double volume,
                long long genus = 0, double within = 2e-3) {
  const Seam& seam = lib.seams.front();
  const vs::mesh::VolumeMesh a = mesh_of(inner, kInner, seam);
  const vs::mesh::VolumeMesh b = mesh_of(outer, kOuter, seam);
  const Mesh both[] = {a.mesh, b.mesh};
  const vs::mesh::WeldResult r = vs::mesh::weld(both, lib);
  INFO(describe(r.diagnostics));
  CHECK(r.ok());
  CHECK(r.topology.watertight());
  CHECK(r.topology.manifold());
  CHECK(r.topology.shells == 1);
  CHECK(r.topology.euler_characteristic() == 2 - 2 * genus);
  CHECK(r.internal_triangles >= 2 * seam.triangles.size());
  CHECK_THAT(vs::mesh::enclosed_volume(r.mesh), WithinRel(volume, within));
}

// Every undirected edge of the triangles, with how many use it.
std::map<std::pair<std::uint32_t, std::uint32_t>, int> edge_counts(
    const std::vector<Triangle>& triangles) {
  std::map<std::pair<std::uint32_t, std::uint32_t>, int> out;
  for (const Triangle& t : triangles) {
    for (std::size_t k = 0; k < 3; ++k) {
      const std::uint32_t a = t[k], b = t[(k + 1) % 3];
      ++out[{std::min(a, b), std::max(a, b)}];
    }
  }
  return out;
}

}  // namespace

TEST_CASE("Cylinder seams lie on the cylinder and keep their chords close to it",
          "[mesh][seam][curved][property]") {
  std::mt19937_64 rng(20);
  std::uniform_real_distribution<double> u(-1, 1);
  for (int i = 0; i < 120; ++i) {
    const dvec3 axis = vs::core::normalize(dvec3{u(rng), u(rng), u(rng)});
    const vs::core::dtransform move =
        vs::core::dtransform::translate({500 * u(rng), 500 * u(rng), 500 * u(rng)}) *
        vs::core::dtransform::rotate(axis, kPi * u(rng));
    vs::sketch::SketchPlane frame;
    frame.origin = move.apply_point({0, 0, 0});
    frame.x_axis = move.apply_vector({1, 0, 0});
    frame.y_axis = move.apply_vector({0, 1, 0});
    const double r = 2 + 60 * std::abs(u(rng));
    const bool band = i % 3 == 0;
    const double sweep = band ? 2 * kPi : 0.2 + 5.9 * std::abs(u(rng));
    const double a0 = kPi * u(rng);
    const double y0 = 20 * u(rng);
    const double len = 1 + 40 * std::abs(u(rng));
    INFO("case " << i << ": r " << r << ", sweep " << sweep << ", length " << len);
    const SeamOutline o = cylinder_outline(frame, r, a0, sweep, y0, y0 + len);
    const Seam seam = seam_for("Curve", o);
    CHECK(seam.curved());
    CHECK(seam.band == band);
    CHECK(seam.spacing <= std::sqrt(8 * r * vs::mesh::kSeamDeflection) / 1.5 + 1e-12);

    // On the cylinder, exactly round-tripping through uv.
    for (const auto& v : seam.vertices) {
      CHECK(std::abs(seam.offset(v.position)) < 1e-9 * (1 + r));
      CHECK(vs::core::length(seam.to_model(v.uv) - v.position) < 1e-9 * (1 + r));
    }
    // The unrolled area is the surface's.
    CHECK_THAT(seam.area(), WithinRel(r * sweep * len, 1e-9));
    // Facing away from the axis; chords within the deflection of it.
    double worst = 0;
    for (const Triangle& t : seam.triangles) {
      const dvec3& a = seam.vertices[t[0]].position;
      const dvec3& b = seam.vertices[t[1]].position;
      const dvec3& c = seam.vertices[t[2]].position;
      const dvec3 centre = (a + b + c) / 3.0;
      CHECK(vs::core::dot(vs::core::cross(b - a, c - a), seam.normal_at(centre)) > 0);
      for (const auto& [p, q] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
        worst = std::max(worst, -seam.offset((p + q) / 2.0));
      }
    }
    CHECK(worst <= vs::mesh::kSeamDeflection * (1 + 1e-6));

    // A closed ring (band) or a disc: edges inside used twice, the loops'
    // edges once, and nothing else open.
    std::set<std::pair<std::uint32_t, std::uint32_t>> loop_edges;
    for (const auto& loop : seam.loops) {
      for (std::size_t k = 0; k < loop.vertices.size(); ++k) {
        const std::uint32_t p = loop.vertices[k], q = loop.vertices[(k + 1) % loop.vertices.size()];
        loop_edges.insert({std::min(p, q), std::max(p, q)});
      }
    }
    CHECK(seam.loops.size() == (band ? 2u : 1u));
    CHECK(seam.hole_fills.empty());
    for (const auto& [e, n] : edge_counts(seam.triangles)) {
      CHECK(n == (loop_edges.contains(e) ? 1 : 2));
    }
    std::size_t outline = 0;
    for (const auto& loop : seam.loops) outline += loop.vertices.size();
    CHECK(outline == seam.outline_vertex_count);

    // Deterministic, and every id distinct.
    const Seam again = seam_for("Curve", o);
    CHECK(again.key == seam.key);
    CHECK(again.triangles == seam.triangles);
    std::set<Id> ids;
    for (const auto& v : seam.vertices) ids.insert(v.id);
    CHECK(ids.size() == seam.vertices.size());
  }
}

TEST_CASE("A cylinder seam and a planar one never share ids", "[mesh][seam][curved]") {
  const vs::sketch::SketchPlane frame;
  const Seam curved = seam_for("Same", cylinder_outline(frame, 10, 0, kPi / 2, 0, 10), 1.0);
  SeamOutline flat{frame, {OutlineLoop{{OutlineSegment::line({0, 0}, {10, 0}),
                                       OutlineSegment::line({10, 0}, {10, 10}),
                                       OutlineSegment::line({10, 10}, {0, 10}),
                                       OutlineSegment::line({0, 10}, {0, 0})}}}};
  const Seam plane = seam_for("Same", flat, 1.0);
  CHECK(curved.key != plane.key);
  CHECK_FALSE(plane.curved());
}

TEST_CASE("Badly shaped cylinder outlines are refused", "[mesh][seam][curved]") {
  const vs::sketch::SketchPlane frame;
  const SubjectRef s{SubjectKind::ConnectionSurface, Id::derive(Id::null(), "Bad"), "Bad"};
  SeamOutline o = cylinder_outline(frame, 10, 0, 2 * kPi, 0, 10);
  o.loops[1].segments[0].start.y += 1.0;  // a helix, not a line along x
  auto seam = vs::mesh::make_seam(s, o, 1.0, kSettings);
  REQUIRE_FALSE(seam.ok());
  CHECK(seam.error().code == "mesh.seam.outline");
}

TEST_CASE("A shaft bonded in a sleeve welds across a band", "[mesh][tessellate][curved]") {
  const Solid shaft = cylinder({0, 0, 0}, 10, 30);
  const Solid sleeve = minus(cylinder({0, 0, 0}, 20, 30), cylinder({0, 0, -1}, 10, 32));
  const vs::sketch::SketchPlane frame;
  SECTION("the band is the whole bore") {
    const SeamLibrary lib = library(seam_for("Bore", cylinder_outline(frame, 10, 0, 2 * kPi, 0, 30)));
    check_pair(shaft, sleeve, lib, kPi * 400 * 30);
  }
  SECTION("the band starts at another angle than the solids' own seam lines") {
    const SeamLibrary lib =
        library(seam_for("Bore", cylinder_outline(frame, 10, 2.1, 2 * kPi, 0, 30), 1.3));
    check_pair(shaft, sleeve, lib, kPi * 400 * 30);
  }
  SECTION("the shaft runs past the sleeve at both ends") {
    const Solid long_shaft = cylinder({0, 0, -10}, 10, 50);
    const SeamLibrary lib = library(seam_for("Bore", cylinder_outline(frame, 10, 0, 2 * kPi, 0, 30)));
    check_pair(long_shaft, sleeve, lib, kPi * 400 * 30 + kPi * 100 * 20);
  }
}

TEST_CASE("A curved panel welds across a partial patch", "[mesh][tessellate][curved]") {
  const vs::sketch::SketchPlane frame;
  const Solid below = box({-50, -50, -1}, {50, 0, 31});  // y < 0
  SECTION("the patch is the whole face of both") {
    const Solid half = minus(cylinder({0, 0, 0}, 10, 30), below);
    const Solid shell = minus(minus(cylinder({0, 0, 0}, 20, 30), cylinder({0, 0, -1}, 10, 32)), below);
    const SeamLibrary lib = library(seam_for("Panel", cylinder_outline(frame, 10, 0, kPi, 0, 30)));
    check_pair(half, shell, lib, kPi * 400 * 30 / 2);
  }
  SECTION("the patch is half of a whole shaft's face") {
    const Solid shaft = cylinder({0, 0, 0}, 10, 30);
    const Solid shell = minus(minus(cylinder({0, 0, 0}, 20, 30), cylinder({0, 0, -1}, 10, 32)), below);
    const SeamLibrary lib = library(seam_for("Panel", cylinder_outline(frame, 10, 0, kPi, 0, 30)));
    check_pair(shaft, shell, lib, kPi * 100 * 30 + kPi * 300 * 30 / 2);
  }
  SECTION("a quarter patch in the middle of a shaft's face, under a curved pad") {
    const Solid shaft = cylinder({0, 0, 0}, 10, 30);
    // A pad round a quarter of the shaft, z 10..20.
    const Solid ring = minus(cylinder({0, 0, 10}, 14, 10), cylinder({0, 0, 9}, 10, 12));
    const Solid pad = minus(minus(ring, box({-50, -50, 0}, {50, 0, 40})),
                            box({-50, -1, 0}, {0, 50, 40}));  // x > 0, y > 0
    const SeamLibrary lib =
        library(seam_for("Pad", cylinder_outline(frame, 10, 0, kPi / 2, 10, 20)));
    check_pair(shaft, pad, lib, kPi * 100 * 30 + kPi * (196 - 100) * 10 / 4);
  }
}

TEST_CASE("Curved seams in any orientation, far from the origin",
          "[mesh][tessellate][curved][property]") {
  std::mt19937_64 rng(21);
  std::uniform_real_distribution<double> u(-1, 1);
  for (int i = 0; i < 24; ++i) {
    const dvec3 axis = vs::core::normalize(dvec3{u(rng), u(rng), u(rng)});
    const vs::core::dtransform move =
        vs::core::dtransform::translate({400 * u(rng), 400 * u(rng), 400 * u(rng)}) *
        vs::core::dtransform::rotate(axis, kPi * u(rng));
    const auto moved = [&](const Solid& s) { return vs::kernel::transformed(s, move).value(); };
    vs::sketch::SketchPlane frame;
    frame.origin = move.apply_point({0, 0, 0});
    frame.x_axis = move.apply_vector({1, 0, 0});
    frame.y_axis = move.apply_vector({0, 1, 0});
    const double r = 3 + 8 * std::abs(u(rng));
    const double wall = 2 + 4 * std::abs(u(rng));
    const double a0 = kPi * u(rng);
    INFO("case " << i << ", r " << r);
    const Solid sleeve =
        minus(cylinder({0, 0, 0}, r + wall, 12), cylinder({0, 0, -1}, r, 14));
    if (i % 2 == 0) {
      const SeamLibrary lib = library(seam_for("Bore", cylinder_outline(frame, r, a0, 2 * kPi, 0, 12)));
      check_pair(moved(cylinder({0, 0, 0}, r, 12)), moved(sleeve), lib,
                 kPi * (r + wall) * (r + wall) * 12);
    } else {
      // A half sleeve on a whole shaft.
      const Solid below = box({-50, -50, -1}, {50, 0, 13});
      const SeamLibrary lib = library(seam_for("Half", cylinder_outline(frame, r, 0, kPi, 0, 12)));
      check_pair(moved(cylinder({0, 0, 0}, r, 12)), moved(minus(sleeve, below)), lib,
                 kPi * r * r * 12 + kPi * ((r + wall) * (r + wall) - r * r) * 12 / 2);
    }
  }
}

TEST_CASE("A volume that covers only part of a curved patch can't conform",
          "[mesh][tessellate][curved]") {
  const vs::sketch::SketchPlane frame;
  const Solid shaft = cylinder({0, 0, 0}, 10, 30);
  // The band runs past the shaft's top.
  const Seam seam = seam_for("Overhang", cylinder_outline(frame, 10, 0, 2 * kPi, 20, 40));
  const Seam* seams[] = {&seam};
  const auto m = vs::mesh::tessellate(shaft, kInner, seams, kSettings, {kFine});
  INFO(describe(m.diagnostics));
  REQUIRE(m.diagnostics.has_errors());
  CHECK(codes(m.diagnostics).find("mesh.tessellate.seam_uncovered") != std::string::npos);
  CHECK(m.seams.empty());
}

TEST_CASE("Documents with curved surfaces weld through the seam library",
          "[mesh][combine][curved]") {
  for (const bool half : {false, true}) {
    curved::Options o;
    o.half = half;
    if (!half) {
      o.patch_start = 5;  // a band on part of the bore: the rest is plain contact
      o.patch_length = 20;
    }
    const auto c = curved::make_curved(o);
    const auto& doc = c.project.document;
    const auto b = vs::model::build(doc, kSettings);
    INFO((half ? "half shell: " : "shaft in sleeve: ") << describe(b.diagnostics));
    REQUIRE(b.ok());
    vs::mesh::CombineOptions opts;
    opts.placeholders = true;
    opts.tessellation.params = kFine;
    const auto m = vs::mesh::combine(doc, b, &c.project.blobs, kSettings, opts);
    INFO(describe(m.diagnostics));
    REQUIRE(m.seams.seams.size() == 1);
    CHECK(m.seams.seams[0].curved());
    CHECK(m.seams.seams[0].band == !half);
    CHECK(m.ok());
    CHECK(m.weld.topology.watertight());
    CHECK(m.weld.topology.manifold());
    const double whole = kPi * 400 * 30;
    CHECK_THAT(vs::mesh::enclosed_volume(m.weld.mesh), WithinRel(half ? whole / 2 : whole, 2e-3));
  }
}

// --- H4 (decision 0026) ------------------------------------------------------------

TEST_CASE("Coarse band seams keep every triangle under half a turn",
          "[mesh][seam][curved][hardening]") {
  // A spacing set coarser than the band's circumference left no lattice
  // inside it: triangles reached from one side of the cut to the other,
  // more than half a turn round the axis.
  const vs::sketch::SketchPlane frame;
  for (const double r : {2.0, 10.0}) {
    for (const double spacing : {0.3 * r, 1.0 * r, 3.0 * r, 10.0 * r, 100.0 * r}) {
      for (const double height : {0.2 * r, 3.0 * r, 20.0 * r, 60.0 * r}) {
        INFO("r " << r << ", spacing " << spacing << ", height " << height);
        const Seam seam = seam_for("Band", cylinder_outline(frame, r, 0.4, 2 * kPi, 0, height), spacing);
        CHECK_THAT(seam.area(), WithinRel(2 * kPi * r * height, 1e-9));
        for (const Triangle& t : seam.triangles) {
          const auto q = seam.triangle_uv(t);
          const double lo = std::min({q[0].x, q[1].x, q[2].x});
          const double hi = std::max({q[0].x, q[1].x, q[2].x});
          CHECK(hi - lo < kPi * r);
          CHECK(vs::core::cross(q[1] - q[0], q[2] - q[0]) > 0);
        }
      }
    }
  }
}

TEST_CASE("A shaft bonded in a sleeve welds across a coarse band",
          "[mesh][tessellate][curved][hardening]") {
  // A thin shaft, long against its circumference.
  const Solid shaft = cylinder({0, 0, 0}, 2, 120);
  const Solid sleeve = minus(cylinder({0, 0, 0}, 5, 120), cylinder({0, 0, -1}, 2, 122));
  const vs::sketch::SketchPlane frame;
  for (const double spacing : {8.0, 40.0, 200.0}) {
    INFO("spacing " << spacing);
    const SeamLibrary lib =
        library(seam_for("Bore", cylinder_outline(frame, 2, 0.3, 2 * kPi, 0, 120), spacing));
    check_pair(shaft, sleeve, lib, kPi * 25 * 120);
  }
}

TEST_CASE("A band's cut may lie on the solids' own seam lines",
          "[mesh][tessellate][curved][hardening]") {
  // Rotated so the shaft's and the sleeve's seam lines run along the band's
  // cut. The band was imprinted as one whole turn, whose own seam edge then
  // lay on the solids' seam lines, and the solids came back unsplit.
  for (const double a0 : {0.7, 2.1, -1.3, kPi, -2.9}) {
    INFO("a0 " << a0);
    const auto turn = [&](const Solid& s) {
      return vs::kernel::transformed(s, vs::core::dtransform::rotate({0, 0, 1}, a0)).value();
    };
    const Solid shaft = turn(cylinder({0, 0, -10}, 10, 50));
    const Solid sleeve = turn(minus(cylinder({0, 0, 0}, 20, 30), cylinder({0, 0, -1}, 10, 32)));
    const vs::sketch::SketchPlane frame;
    const SeamLibrary lib =
        library(seam_for("Bore", cylinder_outline(frame, 10, a0, 2 * kPi, 0, 30), 1.7));
    check_pair(shaft, sleeve, lib, kPi * 400 * 30 + kPi * 100 * 20);
  }
}

TEST_CASE("A patch round nearly all of a shaft leaves its small remainder outside",
          "[mesh][tessellate][curved][hardening]") {
  // The remainder of the shaft's face is a sliver of a few degrees, at
  // most one piece of BRepMesh's wide, whose points all lie on the patch's
  // edges. The audit expected it to be taken for a piece of the patch; it
  // isn't (BRepMesh always divides its arcs), so this guards it.
  for (const double r : {2.0, 10.0}) {
    for (const double w : {0.01, 0.15}) {
      INFO("r " << r << ", slot " << w);
      const double half = std::asin(w / r);
      const double R = r + 3;
      const Solid shaft = cylinder({0, 0, 0}, r, 30);
      // A ring round the shaft, less a slot |y| < w on the -x side.
      const Solid ring = minus(cylinder({0, 0, 10}, R, 10), cylinder({0, 0, 9}, r, 12));
      const Solid pad = minus(ring, box({-50, -w, 0}, {0, w, 40}));
      const vs::sketch::SketchPlane frame;
      const SeamLibrary lib = library(seam_for(
          "Wrap", cylinder_outline(frame, r, -kPi + half, 2 * kPi - 2 * half, 10, 20), 0.5));
      // The slot's share of a disc of radius q: |y| < w, x < 0.
      const auto slot = [&](double q) { return w * std::sqrt(q * q - w * w) + q * q * std::asin(w / q); };
      // A thin shaft loses a few tenths of a percent to its chords.
      check_pair(shaft, pad, lib,
                 kPi * r * r * 30 + (kPi * (R * R - r * r) - slot(R) + slot(r)) * 10, 0, 5e-3);
    }
  }
}

TEST_CASE("A patch the kernel can't imprint reports why",
          "[mesh][tessellate][curved][hardening]") {
  // A band thinner than the minimum feature size: the seam is made, but
  // its face isn't. The kernel's reason used to be dropped, leaving only
  // "no face covers the patch".
  const vs::sketch::SketchPlane frame;
  const Solid shaft = cylinder({0, 0, 0}, 10, 30);
  const double thin = 0.5 * kSettings.min_feature_size_mm();
  const SubjectRef s{SubjectKind::ConnectionSurface, Id::derive(Id::null(), "Thin"), "Thin"};
  auto seam = vs::mesh::make_seam(s, cylinder_outline(frame, 10, 0, 2 * kPi, 10, 10 + thin), 1.0,
                                  kSettings);
  INFO((seam.ok() ? std::string() : seam.error().to_string()));
  REQUIRE(seam.ok());
  const Seam* seams[] = {&*seam};
  const auto m = vs::mesh::tessellate(shaft, kInner, seams, kSettings, {kFine});
  INFO(describe(m.diagnostics));
  REQUIRE(m.diagnostics.has_errors());
  CHECK(codes(m.diagnostics).find("kernel.") != std::string::npos);
}

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "vs/core/settings.hpp"
#include "vs/kernel/kernel.hpp"
#include "vs/mesh/seam_library.hpp"
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

namespace {

const auto kSettings = vs::core::ProductSettings::defaults();
constexpr double kPi = std::numbers::pi;
const SubjectRef kLower{SubjectKind::Volume, Id::derive(Id::null(), "Lower"), "Lower"};
const SubjectRef kUpper{SubjectKind::Volume, Id::derive(Id::null(), "Upper"), "Upper"};
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

// The plane z = `z`, normal +z.
vs::sketch::SketchPlane plane_at(double z) {
  vs::sketch::SketchPlane p;
  p.origin = {0, 0, z};
  return p;
}

OutlineLoop rect(dvec2 lo, dvec2 hi) {
  OutlineLoop l;
  const dvec2 c[4] = {lo, {hi.x, lo.y}, hi, {lo.x, hi.y}};
  for (int i = 0; i < 4; ++i) l.segments.push_back(OutlineSegment::line(c[i], c[(i + 1) % 4]));
  return l;
}

OutlineLoop circle(dvec2 c, double r, double start = 0.0) {
  OutlineLoop l;
  l.segments.push_back(OutlineSegment::arc(c, r, start, 2 * kPi));
  return l;
}

Seam seam_for(const char* name, const SeamOutline& outline, double spacing) {
  const SubjectRef s{SubjectKind::ConnectionSurface, Id::derive(Id::null(), name), name};
  auto seam = vs::mesh::make_seam(s, outline, spacing, kSettings);
  REQUIRE(seam.ok());
  seam->volumes = {kLower.id, kUpper.id};
  return std::move(seam).value();
}

SeamLibrary library(Seam seam) {
  SeamLibrary lib;
  lib.volume_names = {{kLower.id, "Lower"}, {kUpper.id, "Upper"}};
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
void check_pair(const Solid& lower, const Solid& upper, const SeamLibrary& lib, double volume,
                long long genus = 0, std::size_t shells = 1, double rel = 2e-3) {
  const Seam& seam = lib.seams.front();
  const vs::mesh::VolumeMesh a = mesh_of(lower, kLower, seam);
  const vs::mesh::VolumeMesh b = mesh_of(upper, kUpper, seam);
  const Mesh both[] = {a.mesh, b.mesh};
  const vs::mesh::WeldResult r = vs::mesh::weld(both, lib);
  INFO(describe(r.diagnostics));
  CHECK(r.ok());
  CHECK(r.topology.watertight());
  CHECK(r.topology.manifold());
  CHECK(r.topology.shells == shells);
  CHECK(r.topology.euler_characteristic() ==
        2 * static_cast<long long>(shells) - 2 * genus);
  CHECK(r.internal_triangles >= 2 * seam.triangles.size());
  CHECK_THAT(vs::mesh::enclosed_volume(r.mesh), WithinRel(volume, rel));
}

}  // namespace

TEST_CASE("A block on a plate: planar faces on and beside the seam weld",
          "[mesh][tessellate]") {
  // The patch is a strip along one edge of the plate's top (as in the
  // bracket): the plate's top face is larger than the patch and shares
  // three of its sides' lines.
  const Solid plate = box({0, 0, 0}, {60, 40, 10});
  const Solid block = box({0, 0, 10}, {60, 10, 50});
  SeamOutline o{plane_at(10), {rect({0, 0}, {60, 10})}};
  const SeamLibrary lib = library(seam_for("Strip", o, 2.5));
  check_pair(plate, block, lib, 60.0 * 40 * 10 + 60.0 * 10 * 40);
}

TEST_CASE("A cylinder on a plate: the curved wall meets the seam's circle",
          "[mesh][tessellate]") {
  const Solid plate = box({-20, -20, -5}, {20, 20, 0});
  const Solid post = cylinder({0, 0, 0}, 8, 15);
  SeamOutline o{plane_at(0), {circle({0, 0}, 8)}};
  SECTION("coarse spacing") {
    // 17 pieces round the circle: the seam's chords cut 2% off the post.
    check_pair(plate, post, library(seam_for("Disc", o, 3.0)), 40.0 * 40 * 5 + kPi * 64 * 15, 0, 1,
               5e-3);
  }
  SECTION("fine spacing") {
    check_pair(plate, post, library(seam_for("Disc", o, 0.4)), 40.0 * 40 * 5 + kPi * 64 * 15);
  }
  SECTION("the circle's seam starts elsewhere: the wall's vertex snaps") {
    SeamOutline turned{plane_at(0), {circle({0, 0}, 8, 0.37)}};
    const SeamLibrary lib = library(seam_for("Disc", turned, 2.0));
    check_pair(plate, post, lib, 40.0 * 40 * 5 + kPi * 64 * 15);
    const Seam* seams[] = {&lib.seams.front()};
    const auto m = vs::mesh::tessellate(post, kUpper, seams, kSettings, {kFine});
    CHECK(codes(m.diagnostics).find("mesh.tessellate.snapped") != std::string::npos);
  }
}

TEST_CASE("A cone and a dome on a plate", "[mesh][tessellate]") {
  const Solid plate = box({-20, -20, -5}, {20, 20, 0});
  SECTION("cone frustum") {
    const auto profile = vs::kernel::make_face(
        vs::kernel::Loop::polygon(std::vector<dvec3>{{0, 0, 0}, {9, 0, 0}, {4, 0, 12}, {0, 0, 12}}),
        kSettings);
    REQUIRE(profile.ok());
    const auto cone = vs::kernel::make_revolution(*profile, {{0, 0, 0}, {0, 0, 1}}, 2 * kPi, kSettings);
    REQUIRE(cone.ok());
    SeamOutline o{plane_at(0), {circle({0, 0}, 9)}};
    const double frustum = kPi * 12 / 3 * (81 + 36 + 16);
    check_pair(plate, *cone, library(seam_for("Disc", o, 1.5)), 40.0 * 40 * 5 + frustum);
  }
  SECTION("hemisphere, with a pole") {
    const auto ball = vs::kernel::make_sphere({0, 0, 0}, 10, kSettings);
    REQUIRE(ball.ok());
    const Solid dome = vs::kernel::common(*ball, box({-11, -11, 0}, {11, 11, 11})).value();
    SeamOutline o{plane_at(0), {circle({0, 0}, 10)}};
    check_pair(plate, dome, library(seam_for("Disc", o, 1.0)),
               40.0 * 40 * 5 + 2.0 / 3 * kPi * 1000);
  }
}

TEST_CASE("A block with filleted corners: arcs and lines in the seam", "[mesh][tessellate]") {
  const Solid plate = box({-5, -5, -4}, {35, 25, 0});
  Solid block = box({0, 0, 0}, {30, 20, 10});
  std::vector<vs::kernel::Edge> vertical;
  for (const auto& e : vs::kernel::edges(block)) {
    const auto pts = vs::kernel::vertex_points(e);
    if (pts.size() == 2 && std::abs(pts[0].z - pts[1].z) > 5) vertical.push_back(e);
  }
  REQUIRE(vertical.size() == 4);
  const double r = 3;
  block = vs::kernel::fillet(block, vertical, r).value();
  OutlineLoop rounded;
  const auto arc = [&](dvec2 c, double a0) {
    rounded.segments.push_back(OutlineSegment::arc(c, r, a0, kPi / 2));
  };
  rounded.segments.push_back(OutlineSegment::line({r, 0}, {30 - r, 0}));
  arc({30 - r, r}, -kPi / 2);
  rounded.segments.push_back(OutlineSegment::line({30, r}, {30, 20 - r}));
  arc({30 - r, 20 - r}, 0);
  rounded.segments.push_back(OutlineSegment::line({30 - r, 20}, {r, 20}));
  arc({r, 20 - r}, kPi / 2);
  rounded.segments.push_back(OutlineSegment::line({0, 20 - r}, {0, r}));
  arc({r, r}, kPi);
  SeamOutline o{plane_at(0), {rounded}};
  const double area = 30.0 * 20 - (4 - kPi) * r * r;
  check_pair(plate, block, library(seam_for("Rounded", o, 1.2)), 40.0 * 30 * 4 + area * 10);
}

TEST_CASE("Bolt holes: filled where neither side has them, walls where they do",
          "[mesh][tessellate]") {
  const Solid lower = box({0, 0, 0}, {60, 20, 10});
  const Solid upper = box({0, 0, 10}, {60, 20, 30});
  SeamOutline o{plane_at(10), {rect({0, 0}, {60, 20}), circle({15, 10}, 2.25), circle({45, 10}, 2.25)}};
  const SeamLibrary lib = library(seam_for("Flange", o, 2.0));
  const double hole = kPi * 2.25 * 2.25;
  SECTION("no holes in either: the fills pair off") {
    check_pair(lower, upper, lib, 60.0 * 20 * 30);
  }
  SECTION("through holes in both") {
    Solid a = minus(minus(lower, cylinder({15, 10, -1}, 2.25, 12)), cylinder({45, 10, -1}, 2.25, 12));
    Solid b = minus(minus(upper, cylinder({15, 10, 9}, 2.25, 22)), cylinder({45, 10, 9}, 2.25, 22));
    check_pair(a, b, lib, 60.0 * 20 * 30 - 2 * hole * 30, 2);
  }
  SECTION("blind holes in the upper one only: the lower one's fills close two voids") {
    Solid b = minus(minus(upper, cylinder({15, 10, 9}, 2.25, 9)), cylinder({45, 10, 9}, 2.25, 9));
    check_pair(lower, b, lib, 60.0 * 20 * 30 - 2 * hole * 8, 0, 3);
  }
}

TEST_CASE("Seams in any orientation, far from the origin", "[mesh][tessellate][property]") {
  // Random rigid motions of three pairs: a cylinder, a cone frustum, and a
  // block with a through hole, each on a plate with a hole where needed.
  std::mt19937_64 rng(8);
  std::uniform_real_distribution<double> u(-1, 1);
  for (int i = 0; i < 45; ++i) {
    const dvec3 axis = vs::core::normalize(dvec3{u(rng), u(rng), u(rng)});
    const double angle = kPi * u(rng);
    const dvec3 shift{400 * u(rng), 400 * u(rng), 400 * u(rng)};
    const vs::core::dtransform move =
        vs::core::dtransform::translate(shift) * vs::core::dtransform::rotate(axis, angle);
    const auto moved = [&](const Solid& s) { return vs::kernel::transformed(s, move).value(); };
    const double r = 3 + 5 * std::abs(u(rng));
    const double spacing = 0.3 + 2 * std::abs(u(rng));
    const double start = 2 * kPi * std::abs(u(rng));
    vs::sketch::SketchPlane frame;
    frame.origin = move.apply_point({0, 0, 0});
    frame.x_axis = move.apply_vector({1, 0, 0});
    frame.y_axis = move.apply_vector({0, 1, 0});
    INFO("case " << i << ", r " << r << ", spacing " << spacing);
    const Solid plate = box({-20, -20, -5}, {20, 20, 0});
    switch (i % 3) {
      case 0: {
        SeamOutline o{frame, {circle({0, 0}, r, start)}};
        check_pair(moved(plate), moved(cylinder({0, 0, 0}, r, 10)),
                   library(seam_for("Disc", o, spacing)), 40.0 * 40 * 5 + kPi * r * r * 10);
        break;
      }
      case 1: {
        const auto profile = vs::kernel::make_face(
            vs::kernel::Loop::polygon(
                std::vector<dvec3>{{0, 0, 0}, {r, 0, 0}, {r / 2, 0, 10}, {0, 0, 10}}),
            kSettings);
        const auto cone =
            vs::kernel::make_revolution(*profile, {{0, 0, 0}, {0, 0, 1}}, 2 * kPi, kSettings);
        REQUIRE(cone.ok());
        SeamOutline o{frame, {circle({0, 0}, r, start)}};
        const double frustum = kPi * 10 / 3 * (r * r + r * r / 2 + r * r / 4);
        check_pair(moved(plate), moved(*cone), library(seam_for("Disc", o, spacing)),
                   40.0 * 40 * 5 + frustum);
        break;
      }
      default: {
        const double w = r + 2, rh = r / 3;
        const Solid lower = minus(plate, cylinder({0, 0, -6}, rh, 7));
        const Solid upper = minus(box({-w, -w, 0}, {w, w, 8}), cylinder({0, 0, -1}, rh, 10));
        SeamOutline o{frame, {rect({-w, -w}, {w, w}), circle({0, 0}, rh, start)}};
        check_pair(moved(lower), moved(upper), library(seam_for("Ring", o, spacing)),
                   40.0 * 40 * 5 + 4 * w * w * 8 - kPi * rh * rh * 13, 1);
      }
    }
  }
}

TEST_CASE("A volume that doesn't cover its patch can't conform", "[mesh][tessellate]") {
  const Solid plate = box({0, 0, 0}, {60, 40, 10});
  SeamOutline o{plane_at(10), {rect({50, 30}, {70, 50})}};  // half off the plate
  const Seam seam = seam_for("Overhang", o, 2.0);
  const Seam* seams[] = {&seam};
  const auto m = vs::mesh::tessellate(plate, kLower, seams, kSettings, {kFine});
  REQUIRE(m.diagnostics.has_errors());
  const auto& d = *std::find_if(m.diagnostics.begin(), m.diagnostics.end(), [](const auto& x) {
    return x.code == "mesh.tessellate.seam_uncovered";
  });
  REQUIRE(d.subjects.size() == 2);
  CHECK(d.subjects[0].id == seam.surface);
  CHECK(d.subjects[1].id == kLower.id);
  // Nowhere near it at all.
  const Solid away = box({100, 100, 100}, {110, 110, 110});
  CHECK(codes(vs::mesh::tessellate(away, kLower, seams, kSettings).diagnostics) ==
        "mesh.tessellate.seam_uncovered");
  // With no seams, any solid tessellates closed.
  const auto plain = vs::mesh::tessellate(plate, kLower, {}, kSettings);
  CHECK(plain.ok());
  CHECK(vs::mesh::analyse(plain.mesh).watertight());
}

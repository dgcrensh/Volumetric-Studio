// Kernel operations added for M4: multi-tool booleans, chamfers, shells,
// sweeps, lofts and cylindrical face queries.

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

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

Solid box(dvec3 lo, dvec3 hi) { return make_box(dbox3::from_min_max(lo, hi), kSettings).value(); }

double volume_of(const Solid& s) {
  const auto m = mass_properties(s);
  REQUIRE(m.ok());
  return m->volume;
}

// An axis-aligned square of side `size` centred on (cx, cy) at height z.
Face square(double cx, double cy, double z, double size) {
  const double h = size / 2;
  const std::array<dvec3, 4> pts{dvec3{cx - h, cy - h, z}, dvec3{cx + h, cy - h, z},
                                 dvec3{cx + h, cy + h, z}, dvec3{cx - h, cy + h, z}};
  return make_face(Loop::polygon(pts), kSettings).value();
}

// The planar face of `solid` whose outward normal is `normal`.
Face face_facing(const Solid& solid, dvec3 normal) {
  for (const Face& f : faces(solid)) {
    const auto p = face_plane(f);
    if (p && vs::core::dot(p->normal, normal) > 1 - 1e-9) return f;
  }
  FAIL("no face facing that way");
  return {};
}

// The edges of `solid` parallel to the z axis.
std::vector<Edge> vertical_edges(const Solid& solid) {
  std::vector<Edge> out;
  for (const Edge& e : edges(solid)) {
    const auto b = bounding_box(e);
    if (b.size().x < 1e-9 && b.size().y < 1e-9) out.push_back(e);
  }
  return out;
}

}  // namespace

TEST_CASE("booleans take several tools at once", "[kernel][boolean]") {
  const Solid plate = box({0, 0, 0}, {40, 10, 5});
  std::vector<Solid> pins;
  for (int i = 0; i < 3; ++i) {
    pins.push_back(make_cylinder(Axis{{5.0 + 15.0 * i, 5, -1}, {0, 0, 1}}, 2, 7, kSettings).value());
  }
  const auto drilled = cut(plate, pins);
  REQUIRE(drilled.ok());
  CHECK(volume_of(*drilled) == Approx(2000 - 3 * kPi * 4 * 5).epsilon(1e-9));

  // Overlapping tools are fine.
  const std::array<Solid, 2> blocks{box({0, 0, 5}, {10, 10, 10}), box({5, 0, 5}, {15, 10, 10})};
  const auto joined = unite(plate, blocks);
  REQUIRE(joined.ok());
  CHECK(volume_of(*joined) == Approx(2000 + 750).epsilon(1e-9));
  CHECK(joined->solid_count() == 1);

  // No tools: the solid comes back unchanged.
  CHECK(unite(plate, std::span<const Solid>{})->is_same(plate));
}

TEST_CASE("chamfers bevel edges", "[kernel][chamfer]") {
  const Solid cube = box({0, 0, 0}, {10, 10, 10});
  const auto edge = vertical_edges(cube);
  REQUIRE(edge.size() == 4);
  const auto one = chamfer(cube, std::span<const Edge>(edge.data(), 1), 2);
  REQUIRE(one.ok());
  CHECK(volume_of(*one) == Approx(1000 - 0.5 * 2 * 2 * 10).epsilon(1e-9));
  CHECK(faces(*one).size() == 7);

  CHECK(chamfer(cube, edge, 0).error().code == "kernel.chamfer.invalid_distance");
  // Two chamfers of 6 mm meet on a 10 mm face: the kernel can't build that.
  const auto too_big = chamfer(cube, edge, 6);
  REQUIRE_FALSE(too_big.ok());
  CHECK(too_big.error().code == "kernel.chamfer.failed");
  const Solid other = box({20, 0, 0}, {30, 10, 10});
  CHECK(chamfer(cube, vertical_edges(other), 1).error().code == "kernel.chamfer.foreign_edge");
}

TEST_CASE("shells hollow a solid", "[kernel][shell]") {
  const Solid cube = box({0, 0, 0}, {20, 20, 20});
  const Face top = face_facing(cube, {0, 0, 1});
  const std::array<Face, 1> open{top};

  const auto inward = shell(cube, open, 2, /*outward=*/false, kSettings);
  REQUIRE(inward.ok());
  CHECK(volume_of(*inward) == Approx(8000 - 16 * 16 * 18).epsilon(1e-9));
  auto box_in = bounding_box(*inward);
  CHECK(box_in.max.x == Approx(20));

  const auto outward = shell(cube, open, 2, /*outward=*/true, kSettings);
  REQUIRE(outward.ok());
  CHECK(volume_of(*outward) == Approx(24 * 24 * 22 - 8000).epsilon(1e-9));
  CHECK(bounding_box(*outward).max.x == Approx(22));

  CHECK(shell(cube, {}, 2, false, kSettings).error().code == "kernel.shell.no_faces");
  CHECK(shell(cube, open, 0, false, kSettings).error().code == "kernel.shell.invalid_thickness");
  const Solid other = box({30, 0, 0}, {40, 10, 10});
  const std::array<Face, 1> foreign{face_facing(other, {0, 0, 1})};
  CHECK(shell(cube, foreign, 2, false, kSettings).error().code == "kernel.shell.foreign_face");
}

TEST_CASE("shells too thick for the body fail with a named diagnostic", "[kernel][shell]") {
  // A 6 mm cylinder can't take 4 mm walls inwards: its inner wall would
  // have a negative radius. (OCCT builds a valid-looking solid for this, so
  // the kernel checks it up front.)
  const Solid rod = make_cylinder(Axis{{0, 0, 0}, {0, 0, 1}}, 3, 20, kSettings).value();
  const std::array<Face, 1> open{face_facing(rod, {0, 0, 1})};
  const auto r = shell(rod, open, 4, false, kSettings);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == "kernel.shell.too_thick");
  // Outwards the same wall only grows, so that works.
  const auto out = shell(rod, open, 4, true, kSettings);
  REQUIRE(out.ok());
  CHECK(volume_of(*out) == Approx(kPi * 49 * 24 - kPi * 9 * 20).epsilon(1e-6));

  // A hole's wall shrinks when shelling outwards.
  const Solid ring = cut(box({-10, -10, 0}, {10, 10, 10}),
                         make_cylinder(Axis{{0, 0, -1}, {0, 0, 1}}, 2, 12, kSettings).value())
                         .value();
  const std::array<Face, 1> ring_top{face_facing(ring, {0, 0, 1})};
  CHECK(shell(ring, ring_top, 3, true, kSettings).error().code == "kernel.shell.too_thick");
}

TEST_CASE("sweeps follow straight and curved paths", "[kernel][sweep]") {
  const Face profile = square(0, 0, 0, 2);
  Path straight;
  straight.segments.emplace_back(LineSegment{{0, 0, 0}, {0, 0, 10}});
  const auto bar = make_sweep(profile, straight, SweepOrientation::FollowPath, kSettings);
  REQUIRE(bar.ok());
  CHECK(volume_of(*bar) == Approx(40).epsilon(1e-9));

  // A quarter turn of radius 20 in the XZ plane: Pappus gives the volume.
  const Face disc =
      make_face(Loop::circle({0, 0, 0}, {0, 0, 1}, 2), kSettings).value();
  Path bend;
  const double s = std::sqrt(0.5);
  bend.segments.emplace_back(
      ArcSegment{{0, 0, 0}, {20 - 20 * s, 0, 20 * s}, {20, 0, 20}});
  const auto elbow = make_sweep(disc, bend, SweepOrientation::FollowPath, kSettings);
  REQUIRE(elbow.ok());
  CHECK(volume_of(*elbow) == Approx(kPi * 4 * (kPi / 2 * 20)).epsilon(1e-6));

  // Fixed orientation keeps the profile flat: a line then a bend makes a
  // solid whose cross-section stays horizontal.
  bend.segments.insert(bend.segments.begin(), LineSegment{{0, 0, -5}, {0, 0, 0}});
  const Face low_disc =
      make_face(Loop::circle({0, 0, -5}, {0, 0, 1}, 2), kSettings).value();
  const auto fixed = make_sweep(low_disc, bend, SweepOrientation::Fixed, kSettings);
  REQUIRE(fixed.ok());
  CHECK(check_validity(*fixed).empty());
}

TEST_CASE("bad sweeps are diagnostics", "[kernel][sweep]") {
  const Face profile = square(0, 0, 0, 2);
  CHECK(make_sweep(profile, Path{}, SweepOrientation::FollowPath, kSettings).error().code ==
        "kernel.sweep.bad_path");
  Path broken;
  broken.segments.emplace_back(LineSegment{{0, 0, 0}, {0, 0, 10}});
  broken.segments.emplace_back(LineSegment{{0, 0, 11}, {0, 0, 20}});
  CHECK(make_sweep(profile, broken, SweepOrientation::FollowPath, kSettings).error().code ==
        "kernel.sweep.bad_path");
  Path flat;
  flat.segments.emplace_back(LineSegment{{0, 0, 0}, {10, 0, 0}});
  CHECK(make_sweep(profile, flat, SweepOrientation::FollowPath, kSettings).error().code ==
        "kernel.sweep.tangent_in_profile");
}

TEST_CASE("lofts join sections", "[kernel][loft]") {
  const std::array<Face, 2> prism{square(0, 0, 0, 10), square(0, 0, 10, 10)};
  const auto block = make_loft(prism, false, kSettings);
  REQUIRE(block.ok());
  CHECK(volume_of(*block) == Approx(1000).epsilon(1e-6));

  // A ruled loft between a 10 mm and a 4 mm square is a frustum.
  const std::array<Face, 2> taper{square(0, 0, 0, 10), square(0, 0, 10, 4)};
  const auto frustum = make_loft(taper, true, kSettings);
  REQUIRE(frustum.ok());
  CHECK(volume_of(*frustum) == Approx(10.0 / 3.0 * (100 + 16 + 40)).epsilon(1e-6));

  // Three sections, smooth: the side runs along the parabola through 10, 6
  // and 10 mm, s(t) = 10 - 8t + 4t² for t = z / 10, so the volume is
  // 10 ∫₀² s(t)² dt = 1104 mm³ (ruled, it would be 1306.7).
  const std::array<Face, 3> three{square(0, 0, 0, 10), square(0, 0, 10, 6),
                                  square(0, 0, 20, 10)};
  const auto waist = make_loft(three, false, kSettings);
  REQUIRE(waist.ok());
  CHECK(volume_of(*waist) == Approx(1104).epsilon(1e-6));
}

TEST_CASE("bad lofts are diagnostics", "[kernel][loft]") {
  const std::array<Face, 1> one{square(0, 0, 0, 10)};
  CHECK(make_loft(one, false, kSettings).error().code == "kernel.loft.too_few_sections");
  const std::array<dvec3, 4> outer{dvec3{-5, -5, 10}, dvec3{5, -5, 10}, dvec3{5, 5, 10},
                                   dvec3{-5, 5, 10}};
  const std::array<Loop, 1> hole{Loop::circle({0, 0, 10}, {0, 0, 1}, 2)};
  const Face ring = make_face(Loop::polygon(outer), hole, kSettings).value();
  const std::array<Face, 2> holed{square(0, 0, 0, 10), ring};
  CHECK(make_loft(holed, false, kSettings).error().code == "kernel.loft.section_has_holes");
}

TEST_CASE("cylindrical faces report their axis, radius and side", "[kernel][queries]") {
  const Solid rod = make_cylinder(Axis{{1, 2, 0}, {0, 0, 1}}, 3, 10, kSettings).value();
  int found = 0;
  for (const Face& f : faces(rod)) {
    const auto c = face_cylinder(f);
    if (!c) continue;
    ++found;
    CHECK(c->radius == Approx(3));
    CHECK(std::abs(c->axis.direction.z) == Approx(1));
    CHECK(c->axis.origin.x == Approx(1));
    CHECK(c->axis.origin.y == Approx(2));
    CHECK_FALSE(c->inward);
    CHECK_FALSE(face_plane(f).has_value());
  }
  CHECK(found == 1);

  const auto drilled = cut(box({-10, -10, 0}, {10, 10, 5}), rod);
  REQUIRE(drilled.ok());
  found = 0;
  for (const Face& f : faces(*drilled)) {
    if (const auto c = face_cylinder(f)) {
      ++found;
      CHECK(c->inward);
    }
  }
  CHECK(found == 1);
}

// Bézier profile segments and sampled surface lookups (decision 0040).

#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/kernel.hpp"

using namespace vs::kernel;
using Catch::Approx;
using vs::core::dvec3;
using vs::core::ProductSettings;

namespace {

const ProductSettings kSettings = ProductSettings::defaults();

// A "D" in the xy plane: a line down x = 0 from (0, 40) to (0, 0), closed
// by a Bézier bulging out to x = 30 (3/5 of its 40 x 40 control box when
// the handles reach x = 40, as here).
Loop dee() {
  Loop loop;
  loop.segments.emplace_back(LineSegment{{0, 40, 0}, {0, 0, 0}});
  loop.segments.emplace_back(BezierSegment{{0, 0, 0}, {40, 0, 0}, {40, 40, 0}, {0, 40, 0}});
  return loop;
}

}  // namespace

TEST_CASE("A Bezier segment evaluates by de Casteljau", "[kernel][bezier]") {
  const BezierSegment b{{0, 0, 0}, {40, 0, 0}, {40, 40, 0}, {0, 40, 0}};
  CHECK(b.at(0) == dvec3{0, 0, 0});
  CHECK(b.at(1) == dvec3{0, 40, 0});
  CHECK(b.at(0.5) == dvec3{30, 20, 0});
  const BezierSegment r = b.reversed();
  CHECK(r.at(0.25) == b.at(0.75));
}

TEST_CASE("A face bounded by a Bezier is exact and valid", "[kernel][bezier]") {
  auto face = make_face(dee(), kSettings);
  REQUIRE(face);
  CHECK(check_validity(*face).empty());
  CHECK(surface_area(*face) == Approx(960).epsilon(1e-9));

  auto prism = make_prism(*face, {0, 0, 10}, kSettings);
  REQUIRE(prism);
  CHECK(check_validity(*prism).empty());
  const auto m = mass_properties(*prism);
  REQUIRE(m.ok());
  CHECK(m->volume == Approx(9600).epsilon(1e-9));

  auto revolved = make_revolution(*face, Axis{{0, 0, 0}, {0, 1, 0}}, std::numbers::pi, kSettings);
  REQUIRE(revolved);
  CHECK(check_validity(*revolved).empty());
}

TEST_CASE("A Bezier shorter than the minimum feature size is refused", "[kernel][bezier]") {
  const double tiny = kSettings.min_feature_size_mm() / 10;
  Loop loop;
  loop.segments.emplace_back(LineSegment{{0, 0, 0}, {10, 0, 0}});
  loop.segments.emplace_back(LineSegment{{10, 0, 0}, {10, 10, 0}});
  loop.segments.emplace_back(LineSegment{{10, 10, 0}, {0, 10, 0}});
  loop.segments.emplace_back(LineSegment{{0, 10, 0}, {0, tiny, 0}});
  loop.segments.emplace_back(
      BezierSegment{{0, tiny, 0}, {0, tiny * 0.6, 0}, {0, tiny * 0.3, 0}, {0, 0, 0}});
  const auto face = make_face(loop, kSettings);
  REQUIRE_FALSE(face);
  CHECK(face.error().code == "kernel.profile.degenerate_segment");
}

TEST_CASE("A sweep follows a Bezier path", "[kernel][bezier]") {
  auto profile = make_face(Loop::circle({0, 0, 0}, {0, 0, 1}, 2), kSettings);
  REQUIRE(profile);
  Path path;
  path.segments.emplace_back(BezierSegment{{0, 0, 0}, {0, 0, 20}, {30, 0, 20}, {30, 0, 40}});
  auto swept = make_sweep(*profile, path, SweepOrientation::FollowPath, kSettings);
  REQUIRE(swept);
  CHECK(check_validity(*swept).empty());
}

TEST_CASE("Sampled surface normals find a Bezier extrusion's side", "[kernel][bezier]") {
  auto face = make_face(dee(), kSettings);
  REQUIRE(face);
  auto prism = make_prism(*face, {0, 0, 10}, kSettings);
  REQUIRE(prism);
  const BezierSegment curve{{0, 0, 0}, {40, 0, 0}, {40, 40, 0}, {0, 40, 0}};
  // On the sketch plane, at the bottom edge, and half way up.
  const std::vector<dvec3> points{curve.at(0.25), curve.at(0.5) + dvec3{0, 0, 5}, curve.at(0.75)};
  int found = 0;
  for (const Face& f : faces(*prism)) {
    const auto normals = surface_normals_at(f, points, kSettings.linear_tolerance_mm);
    if (!normals) continue;
    ++found;
    REQUIRE(normals->size() == 3);
    // The middle of the D's bulge faces +x, out of the material.
    CHECK((*normals)[1].x == Approx(1).margin(1e-9));
    CHECK(std::abs((*normals)[1].z) < 1e-9);
  }
  CHECK(found == 1);
  // A point off the surface matches nothing; neither does an empty list.
  for (const Face& f : faces(*prism)) {
    const std::vector<dvec3> off{curve.at(0.5) + dvec3{0.01, 0, 5}};
    CHECK_FALSE(surface_normals_at(f, off, kSettings.linear_tolerance_mm));
    CHECK_FALSE(surface_normals_at(f, {}, kSettings.linear_tolerance_mm));
  }
}

TEST_CASE("Sampled surface normals follow a plane past its face", "[kernel][bezier]") {
  auto b = make_box(vs::core::dbox3::from_min_max({0, 0, 0}, {10, 10, 10}), kSettings);
  REQUIRE(b);
  // Points on the plane z = 10 but far outside the box's top face.
  const std::vector<dvec3> points{{-50, 3, 10}, {200, -40, 10}};
  int found = 0;
  for (const Face& f : faces(*b)) {
    const auto normals = surface_normals_at(f, points, kSettings.linear_tolerance_mm);
    if (!normals) continue;
    ++found;
    CHECK((*normals)[0].z == Approx(1));
  }
  CHECK(found == 1);
}

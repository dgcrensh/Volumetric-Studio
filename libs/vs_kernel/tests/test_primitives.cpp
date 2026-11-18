#include <array>
#include <numbers>

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
// Analytic comparisons are relative; mass properties of exact B-rep geometry
// are good to far better than this.
constexpr double kRel = 1e-9;

double volume_of(const Result<Solid>& s) {
  REQUIRE(s.ok());
  const auto m = mass_properties(*s);
  REQUIRE(m.ok());
  return m->volume;
}
}  // namespace

TEST_CASE("boxes, cylinders and spheres have their analytic volumes", "[kernel][primitives]") {
  CHECK(volume_of(make_box(dbox3::from_min_max({-1, -2, -3}, {4, 5, 6}), kSettings)) ==
        Approx(5.0 * 7 * 9).epsilon(kRel));
  CHECK(volume_of(make_cylinder({{1, 2, 3}, {1, 1, 0}}, 7, 11, kSettings)) ==
        Approx(kPi * 49 * 11).epsilon(kRel));
  CHECK(volume_of(make_sphere({10, -10, 5}, 12.5, kSettings)) ==
        Approx(4.0 / 3.0 * kPi * 12.5 * 12.5 * 12.5).epsilon(kRel));
}

TEST_CASE("primitives reject sizes below the minimum feature", "[kernel][primitives]") {
  const double tiny = kSettings.min_feature_size_mm() / 2;
  CHECK(make_box(dbox3::from_min_max({0, 0, 0}, {1, 1, tiny}), kSettings).error().code ==
        "kernel.primitive.invalid_size");
  CHECK(make_box(dbox3::empty(), kSettings).error().code == "kernel.primitive.invalid_size");
  CHECK(make_cylinder({}, tiny, 1, kSettings).error().code == "kernel.primitive.invalid_size");
  CHECK(make_cylinder({}, 1, -1, kSettings).error().code == "kernel.primitive.invalid_size");
  CHECK(make_sphere({}, 0, kSettings).error().code == "kernel.primitive.invalid_size");
  CHECK(make_cylinder({{0, 0, 0}, {0, 0, 0}}, 1, 1, kSettings).error().code ==
        "kernel.primitive.bad_axis");
}

TEST_CASE("primitives must fit the working space", "[kernel][primitives]") {
  const double e = kSettings.working_half_extent_mm;
  CHECK(make_box(dbox3::from_min_max({e - 10, 0, 0}, {e, 10, 10}), kSettings).ok());
  CHECK(make_box(dbox3::from_min_max({e - 10, 0, 0}, {e + 1, 10, 10}), kSettings).error().code ==
        "kernel.working_space.exceeded");
  CHECK(make_sphere({e - 5, 0, 0}, 10, kSettings).error().code ==
        "kernel.working_space.exceeded");
}

TEST_CASE("a polygon profile with holes extrudes to the right volume", "[kernel][primitives]") {
  const std::array<dvec3, 4> outer{dvec3{0, 0, 0}, {40, 0, 0}, {40, 30, 0}, {0, 30, 0}};
  // The square hole runs clockwise, the circle anticlockwise: make_face
  // orients both.
  const std::array<dvec3, 4> square{dvec3{25, 10, 0}, {25, 20, 0}, {35, 20, 0}, {35, 10, 0}};
  const std::array<Loop, 2> holes{Loop::circle({10, 15, 0}, {0, 0, 1}, 5),
                                  Loop::polygon(square)};
  const auto face = make_face(Loop::polygon(outer), holes, kSettings);
  REQUIRE(face.ok());
  const double area = 40.0 * 30 - kPi * 25 - 100;
  CHECK(surface_area(*face) == Approx(area).epsilon(kRel));

  const auto prism = make_prism(*face, {0, 0, 8}, kSettings);
  CHECK(volume_of(prism) == Approx(area * 8).epsilon(kRel));
}

TEST_CASE("profiles mixing lines and arcs build faces", "[kernel][primitives]") {
  // A 20 x 10 slot: two straight sides and two semicircular ends.
  Loop slot;
  slot.segments = {LineSegment{{0, 0, 0}, {20, 0, 0}},
                   ArcSegment{{20, 0, 0}, {25, 5, 0}, {20, 10, 0}},
                   LineSegment{{20, 10, 0}, {0, 10, 0}},
                   ArcSegment{{0, 10, 0}, {-5, 5, 0}, {0, 0, 0}}};
  const auto face = make_face(slot, kSettings);
  REQUIRE(face.ok());
  CHECK(surface_area(*face) == Approx(20.0 * 10 + kPi * 25).epsilon(kRel));
}

TEST_CASE("bad profiles are reported by code", "[kernel][primitives]") {
  const std::array<dvec3, 4> square{dvec3{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};

  SECTION("open loop") {
    Loop open = Loop::polygon(square);
    std::get<LineSegment>(open.segments.back()).end = {0, 0.5, 0};
    CHECK(make_face(open, kSettings).error().code == "kernel.profile.open_loop");
  }
  SECTION("non-planar loop") {
    const std::array<dvec3, 4> bent{dvec3{0, 0, 0}, {10, 0, 0}, {10, 10, 3}, {0, 10, 0}};
    CHECK(make_face(Loop::polygon(bent), kSettings).error().code == "kernel.profile.not_planar");
  }
  SECTION("hole outside the outer loop") {
    const std::array<Loop, 1> holes{Loop::circle({30, 5, 0}, {0, 0, 1}, 2)};
    CHECK(make_face(Loop::polygon(square), holes, kSettings).error().code ==
          "kernel.profile.hole_outside");
  }
  SECTION("hole out of plane") {
    const std::array<Loop, 1> holes{Loop::circle({5, 5, 1}, {0, 0, 1}, 2)};
    CHECK(make_face(Loop::polygon(square), holes, kSettings).error().code ==
          "kernel.profile.not_planar");
  }
  SECTION("collinear arc") {
    Loop loop = Loop::polygon(square);
    loop.segments[0] = ArcSegment{{0, 0, 0}, {5, 0, 0}, {10, 0, 0}};
    CHECK(make_face(loop, kSettings).error().code == "kernel.profile.bad_segment");
  }
  SECTION("empty loop") {
    CHECK(make_face(Loop{}, kSettings).error().code == "kernel.profile.empty_loop");
  }
}

TEST_CASE("revolving a rectangle off the axis makes a tube", "[kernel][primitives]") {
  // A rectangle in the XZ plane from r = 5 to r = 8, z = 0 to 10, about Z.
  const std::array<dvec3, 4> rect{dvec3{5, 0, 0}, {8, 0, 0}, {8, 0, 10}, {5, 0, 10}};
  const auto face = make_face(Loop::polygon(rect), kSettings);
  REQUIRE(face.ok());

  const auto tube = make_revolution(*face, {{0, 0, 0}, {0, 0, 1}}, 2 * kPi, kSettings);
  CHECK(volume_of(tube) == Approx(kPi * (64 - 25) * 10).epsilon(kRel));

  const auto quarter = make_revolution(*face, {{0, 0, 0}, {0, 0, 1}}, kPi / 2, kSettings);
  CHECK(volume_of(quarter) == Approx(kPi * (64 - 25) * 10 / 4).epsilon(kRel));

  CHECK(make_revolution(*face, {{6, 0, 0}, {0, 0, 1}}, kPi, kSettings).error().code ==
        "kernel.revolve.axis_crosses_profile");
  CHECK(make_revolution(*face, {{0, 0, 0}, {0, 0, 1}}, 0, kSettings).error().code ==
        "kernel.revolve.bad_angle");
  CHECK(make_revolution(*face, {{0, 0, 0}, {0, 0, 1}}, 7, kSettings).error().code ==
        "kernel.revolve.bad_angle");
}

TEST_CASE("extrusions check their depth", "[kernel][primitives]") {
  const std::array<dvec3, 3> tri{dvec3{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
  const auto face = make_face(Loop::polygon(tri), kSettings);
  REQUIRE(face.ok());
  CHECK(make_prism(*face, {0, 0, 1e-5}, kSettings).error().code ==
        "kernel.primitive.invalid_size");
  CHECK(make_prism(Face{}, {0, 0, 1}, kSettings).error().code == "kernel.primitive.empty_profile");
  CHECK(volume_of(make_prism(*face, {0, 0, -4}, kSettings)) == Approx(200.0).epsilon(kRel));
}

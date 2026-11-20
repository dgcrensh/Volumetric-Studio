// Containment of B-rep and mesh contents.

#include <array>
#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/operations.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/validate/containment.hpp"

using namespace vs::validate;
using vs::core::dbox3;
using vs::core::dvec3;
using vs::core::fvec3;
using vs::core::SubjectKind;
using vs::core::SubjectRef;

namespace {

const auto kSettings = vs::core::ProductSettings::defaults();
const SubjectRef kVolume{SubjectKind::Volume, vs::core::Id::random(), "Part"};

vs::kernel::Solid box(dvec3 lo, dvec3 hi) {
  auto b = vs::kernel::make_box(dbox3::from_min_max(lo, hi), kSettings);
  REQUIRE(b.ok());
  return *b;
}

// The 12 triangles of an axis-aligned box's surface.
struct BoxMesh {
  std::vector<fvec3> positions;
  std::vector<std::uint32_t> indices;
  MeshView view() const { return {positions, indices, {}}; }
};
BoxMesh box_mesh(fvec3 lo, fvec3 hi) {
  BoxMesh m;
  for (int k = 0; k < 8; ++k) {
    m.positions.push_back({(k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y, (k & 4) ? hi.z : lo.z});
  }
  m.indices = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
               2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
  return m;
}

}  // namespace

TEST_CASE("B-rep contents inside, flush with and outside their boundary",
          "[validate][containment]") {
  const auto boundary = box({0, 0, 0}, {20, 20, 20});
  SECTION("inside") {
    CHECK(check_containment(boundary, box({1, 1, 1}, {19, 19, 19}), kVolume, kSettings).contained);
  }
  SECTION("filling it exactly") {
    CHECK(check_containment(boundary, box({0, 0, 0}, {20, 20, 20}), kVolume, kSettings).contained);
  }
  SECTION("crossing by less than the tolerance") {
    CHECK(check_containment(boundary, box({0, 0, 0}, {20 + 5e-7, 20, 20}), kVolume, kSettings)
              .contained);
  }
  SECTION("sticking out") {
    const auto r = check_containment(boundary, box({5, 5, 5}, {25, 15, 15}), kVolume, kSettings);
    CHECK_FALSE(r.contained);
    CHECK(std::abs(r.reach - 5.0) < 1e-6);
    CHECK(std::abs(r.outside_volume - 5.0 * 10 * 10) < 1e-6);
    CHECK(std::abs(r.region.min.x - 20.0) < 1e-6);
    REQUIRE(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].code == "validate.containment");
    CHECK(r.diagnostics[0].subjects.front() == kVolume);
  }
  SECTION("entirely elsewhere") {
    const auto r = check_containment(boundary, box({30, 0, 0}, {40, 10, 10}), kVolume, kSettings);
    CHECK_FALSE(r.contained);
    CHECK(std::abs(r.reach - 20.0) < 1e-6);
  }
}

TEST_CASE("Mesh contents", "[validate][containment][mesh]") {
  const auto boundary = box({0, 0, 0}, {20, 20, 20});
  SECTION("inside, and on the boundary") {
    CHECK(check_containment(boundary, box_mesh({1, 1, 1}, {19, 19, 19}).view(), kVolume,
                            kSettings)
              .contained);
    CHECK(check_containment(boundary, box_mesh({0, 0, 0}, {20, 20, 20}).view(), kVolume,
                            kSettings)
              .contained);
  }
  SECTION("one corner out") {
    auto m = box_mesh({1, 1, 1}, {19, 19, 19});
    m.positions[7] = {22, 19, 19};
    const auto r = check_containment(boundary, m.view(), kVolume, kSettings);
    CHECK_FALSE(r.contained);
    CHECK(r.outside_points >= 1);
    CHECK(std::abs(r.reach - 2.0) < 1e-6);
    CHECK(r.diagnostics[0].code == "validate.containment");
  }
  SECTION("a triangle crossing a concave boundary between its vertices") {
    // An L-shaped boundary (the 20 x 20 square without its 10..20 corner).
    // The triangle's corners sit in both arms; its centre is in the notch.
    auto l = vs::kernel::cut(boundary, box({10, 10, -1}, {21, 21, 21}));
    REQUIRE(l.ok());
    BoxMesh m;
    m.positions = {{19.5f, 9.5f, 10}, {9.5f, 19.5f, 10}, {19.5f, 9, 10}};
    m.indices = {0, 1, 2};
    const auto r = check_containment(*l, m.view(), kVolume, kSettings);
    CHECK_FALSE(r.contained);
    CHECK(r.outside_points == 1);  // the centroid
  }
  SECTION("FP32 rounding of a vertex on the boundary isn't a crossing") {
    // 999.99997 isn't an FP32 number; the nearest is 1000, 3e-5 mm out,
    // far beyond the 1e-6 mm tolerance but within FP32's reach.
    const auto big = box({0, 0, 0}, {999.99997, 10, 10});
    auto m = box_mesh({1, 1, 1}, {static_cast<float>(999.99997), 9, 9});
    CHECK(m.positions[1].x == 1000.0f);
    CHECK(check_containment(big, m.view(), kVolume, kSettings).contained);
  }
  SECTION("bad indices") {
    auto m = box_mesh({1, 1, 1}, {19, 19, 19});
    m.indices.push_back(99);
    m.indices.push_back(0);
    m.indices.push_back(1);
    const auto r = check_containment(boundary, m.view(), kVolume, kSettings);
    CHECK_FALSE(r.contained);
    CHECK(r.diagnostics[0].code == "validate.undecided");
  }
}

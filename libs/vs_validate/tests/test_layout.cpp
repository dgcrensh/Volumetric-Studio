// Layout checks in detail: overlap against touching, structural adjacency,
// keep-outs, face names and the connection type rules.

#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "fixtures.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"
#include "vs/validate/layout.hpp"

using namespace vs::validate;
using namespace vs::model;
using fixtures::add_box;
using fixtures::add_patch;
using fixtures::bolts;
using vs::feature::Param;

namespace {

const auto kSettings = vs::core::ProductSettings::defaults();

LayoutReport check_layout(const Document& doc) {
  const auto errors = check(doc);
  INFO(fixtures::codes(errors));
  REQUIRE_FALSE(errors.has_errors());
  const auto b = build(doc, kSettings);
  INFO(fixtures::codes(b.diagnostics));
  REQUIRE(b.ok());
  return validate_layout(doc, b, kSettings);
}

}  // namespace

TEST_CASE("Penetration up to the tolerance is touching, beyond it overlap",
          "[validate][layout]") {
  const double tol = kSettings.linear_tolerance_mm;
  SECTION("within the tolerance: contact, not overlap") {
    Document doc = make_document("t");
    add_box(doc, "A", {0, 0, 0}, {20, 20, 20});
    add_box(doc, "B", {20 - tol / 2, 0, 0}, {40, 20, 20});
    const auto r = check_layout(doc);
    CHECK(fixtures::codes(r.diagnostics) == "validate.contact");
    CHECK(r.overlaps.empty());
  }
  SECTION("a thousandth of a millimetre in: overlap") {
    Document doc = make_document("t");
    add_box(doc, "A", {0, 0, 0}, {20, 20, 20});
    add_box(doc, "B", {20 - 1e-3, 0, 0}, {40, 20, 20});
    const auto r = check_layout(doc);
    CHECK(fixtures::codes(r.diagnostics) == "validate.overlap");
    REQUIRE(r.overlaps.size() == 1);
    CHECK(std::abs(r.overlaps[0].depth - 1e-3) < 1e-9);
  }
  SECTION("one inside the other: the inner one's whole volume") {
    Document doc = make_document("t");
    add_box(doc, "Outer", {0, 0, 0}, {50, 50, 50});
    add_box(doc, "Inner", {10, 10, 10}, {20, 30, 40});
    const auto r = check_layout(doc);
    REQUIRE(r.overlaps.size() == 1);
    CHECK(std::abs(r.overlaps[0].volume - 10.0 * 20 * 30) < 1e-6);
    CHECK(std::abs(r.overlaps[0].depth - 10.0) < 1e-9);
  }
  SECTION("apart: nothing, and the boxes are never intersected") {
    Document doc = make_document("t");
    add_box(doc, "A", {0, 0, 0}, {20, 20, 20});
    add_box(doc, "B", {30, 0, 0}, {40, 20, 20});
    const auto r = check_layout(doc);
    CHECK(r.diagnostics.empty());
    CHECK(r.pairs_tested == 0);
  }
}

TEST_CASE("Siblings sharing a connection surface are adjacent without a distance test",
          "[validate][layout]") {
  Document doc = make_document("t");
  const Id a = add_box(doc, "A", {0, 0, 0}, {20, 20, 20});
  const Id b = add_box(doc, "B", {0, 0, 20}, {20, 20, 40});
  add_patch(doc, "Joint", {a, b}, 20, 10, 10, 20, 20, BondedParams{});
  CHECK(check_layout(doc).diagnostics.empty());
}

TEST_CASE("Keep-out volumes", "[validate][layout]") {
  Document doc = make_document("t");
  const Id block = add_box(doc, "Block", {0, 0, 0}, {40, 40, 20});
  const Id channel = add_box(doc, "Channel", {10, 10, 10}, {30, 30, 20});
  doc.find_volume(channel)->keep_out = true;

  SECTION("a keep-out that cuts its volume: no overlap, and touching it is fine") {
    doc.keep_outs.push_back({Id::random(), "Channel cut", {channel}, {block}});
    CHECK(check_layout(doc).diagnostics.empty());
  }
  SECTION("a keep-out sitting in a volume it doesn't cut overlaps it") {
    CHECK(fixtures::codes(check_layout(doc).diagnostics) == "validate.overlap");
  }
}

TEST_CASE("Contact names the faces that touch", "[validate][layout]") {
  Document doc = make_document("t");
  add_box(doc, "Low", {0, 0, 0}, {20, 20, 10});
  add_box(doc, "High", {5, 5, 10}, {15, 15, 30});  // sits on top, smaller
  const auto r = check_layout(doc);
  REQUIRE(r.contacts.size() == 1);
  CHECK(r.contacts[0].face_a == "+z");
  CHECK(r.contacts[0].face_b == "-z");
  CHECK(std::abs(r.contacts[0].area - 100.0) < 1e-6);
  CHECK(std::abs(r.contacts[0].region.min.x - 5.0) < 1e-6);
}

TEST_CASE("Connection type rules", "[validate][layout][connection]") {
  Document doc = make_document("t");
  const Id a = add_box(doc, "A", {0, 0, 0}, {40, 20, 10});
  const Id b = add_box(doc, "B", {0, 0, 10}, {40, 20, 20});

  SECTION("a sound bolted flange passes") {
    add_patch(doc, "Flange", {a, b}, 10, 20, 10, 40, 20, bolts("M4", 2, 2, 20, 20, 10));
    CHECK(check_layout(doc).diagnostics.empty());
  }
  SECTION("a patch away from a volume it joins") {
    add_patch(doc, "Flange", {a, b}, 15, 20, 10, 40, 20, BondedParams{});
    const auto r = check_layout(doc);
    // z = 15 is inside B (touching it), 5 mm above A.
    CHECK(fixtures::codes(r.diagnostics) == "validate.connection.off_boundary");
    REQUIRE(r.diagnostics[0].subjects.size() == 2);
    CHECK(r.diagnostics[0].subjects[1].id == a);
  }
  SECTION("a clearance gap below the minimum feature size") {
    ClearanceParams gap;
    gap.gap = Param::of(0);
    add_patch(doc, "Gap", {a, b}, 10, 20, 10, 40, 20, gap);
    CHECK(fixtures::codes(check_layout(doc).diagnostics) == "validate.connection.gap");
  }
  SECTION("a clearance patch may sit within its gap of the volumes") {
    Document apart = make_document("t");
    const Id lo = add_box(apart, "Lo", {0, 0, 0}, {40, 20, 10});
    const Id hi = add_box(apart, "Hi", {0, 0, 11}, {40, 20, 20});
    ClearanceParams gap;
    gap.gap = Param::of(1);
    add_patch(apart, "Gap", {lo, hi}, 10.5, 20, 10, 40, 20, gap);
    CHECK(check_layout(apart).diagnostics.empty());
    gap.gap = Param::of(0.25);
    apart.connections[0].params = gap;
    CHECK(fixtures::codes(check_layout(apart).diagnostics) ==
          "validate.connection.off_boundary validate.connection.off_boundary");
  }
  SECTION("a minimum wall of nothing") {
    add_patch(doc, "Flange", {a, b}, 10, 20, 10, 40, 20, bolts("M4", 0, 2, 20, 20, 10));
    CHECK(fixtures::codes(check_layout(doc).diagnostics) == "validate.connection.min_wall");
  }
  SECTION("a hole pattern off the patch") {
    add_patch(doc, "Flange", {a, b}, 10, 20, 10, 40, 20, bolts("M4", 2, 2, 20, 50, 10));
    const auto r = check_layout(doc);
    CHECK(fixtures::codes(r.diagnostics) == "validate.connection.edge_wall");
    CHECK(r.diagnostics[0].message.find("off the patch") != std::string::npos);
  }
  SECTION("holes on a bolt circle too close together") {
    BoltedFlangeParams p = bolts("M4", 2, 1, 0, 20, 10);
    BoltCircle circle;
    circle.count = Param::of(8);
    circle.diameter = Param::of(11);  // 8 holes 4.2 mm apart; M4 needs 4.5 + 2
    p.holes.layout = circle;
    add_patch(doc, "Flange", {a, b}, 10, 20, 10, 40, 20, p);
    CHECK(fixtures::codes(check_layout(doc).diagnostics) == "validate.connection.hole_spacing");
  }
  SECTION("a connection surface to the outside is checked against its one volume") {
    add_patch(doc, "Mount", {a}, 0, 20, 10, 40, 20, bolts("M4", 2, 2, 20, 20, 10));
    // A and B touch with no surface between them now.
    CHECK(fixtures::codes(check_layout(doc).diagnostics) == "validate.contact");
  }
}

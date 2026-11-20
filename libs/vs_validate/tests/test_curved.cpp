// L2: layout validation of curved connection surfaces. The checks work on
// kernel geometry, so these confirm they hold for cylinders too.

#include <algorithm>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "curved.hpp"
#include "vs/model/build.hpp"
#include "vs/validate/layout.hpp"
#include "vs/validate/validate.hpp"

using namespace vs::model;
using vs::feature::Param;

namespace {

const auto kSettings = ProductSettings::defaults();

std::string codes(const Diagnostics& ds) {
  std::string s;
  for (const auto& d : ds) s += d.code + " ";
  return s;
}

std::size_t count(const Diagnostics& ds, const std::string& code) {
  std::size_t n = 0;
  for (const auto& d : ds) {
    if (d.code == code) ++n;
  }
  return n;
}

vs::validate::LayoutReport layout_of(const Document& doc) {
  const auto b = build(doc, kSettings);
  INFO(codes(b.diagnostics));
  REQUIRE(b.ok());
  return vs::validate::validate_layout(doc, b, kSettings);
}

}  // namespace

TEST_CASE("A bonded band and a bonded half patch validate cleanly", "[validate][curved]") {
  for (const bool half : {false, true}) {
    curved::Options o;
    o.half = half;
    const auto c = curved::make_curved(o);
    const auto r = layout_of(c.project.document);
    INFO(codes(r.diagnostics));
    CHECK(r.diagnostics.empty());
    CHECK(r.overlaps.empty());
    CHECK(r.contacts.empty());
  }
}

TEST_CASE("A band off the shaft is off its boundary", "[validate][curved]") {
  auto c = curved::make_curved();
  std::get<CylinderPatch>(c.project.document.connections[0].geometry).radius = Param::of(12.0);
  const auto r = layout_of(c.project.document);
  INFO(codes(r.diagnostics));
  // Inside the sleeve's wall, two millimetres off the shaft.
  REQUIRE(count(r.diagnostics, "validate.connection.off_boundary") == 1);
  const auto& d = *std::find_if(r.diagnostics.begin(), r.diagnostics.end(), [](const auto& x) {
    return x.code == "validate.connection.off_boundary";
  });
  CHECK(d.subjects.at(0).id == c.surface);
  CHECK(d.subjects.at(1).id == c.shaft);
}

TEST_CASE("A clearance fit round a pin", "[validate][curved]") {
  curved::Options o;
  o.gap = 0.2;
  o.params = ClearanceParams{Param::of(0.2)};
  SECTION("the gap the bore leaves is declared") {
    const auto c = curved::make_curved(o);
    const auto r = layout_of(c.project.document);
    INFO(codes(r.diagnostics));
    CHECK(r.diagnostics.empty());
  }
  SECTION("a gap below the minimum feature size") {
    o.gap = 0.0005;
    o.params = ClearanceParams{Param::of(0.0005)};
    const auto c = curved::make_curved(o);
    const auto r = layout_of(c.project.document);
    INFO(codes(r.diagnostics));
    CHECK(count(r.diagnostics, "validate.connection.gap") == 1);
  }
  SECTION("a declared gap smaller than the bore leaves") {
    o.params = ClearanceParams{Param::of(0.1)};
    const auto c = curved::make_curved(o);
    const auto r = layout_of(c.project.document);
    INFO(codes(r.diagnostics));
    // The patch is on the pin; the bore is 0.2 away, beyond the 0.1 declared.
    CHECK(count(r.diagnostics, "validate.connection.off_boundary") == 1);
  }
}

TEST_CASE("A shaft in a sleeve without a surface is undeclared contact", "[validate][curved]") {
  auto c = curved::make_curved();
  c.project.document.connections.clear();
  const auto r = layout_of(c.project.document);
  INFO(codes(r.diagnostics));
  CHECK(count(r.diagnostics, "validate.contact") >= 1);
  CHECK(r.overlaps.empty());
}

TEST_CASE("Whole-document validation of a curved joint", "[validate][curved]") {
  const auto c = curved::make_curved();
  const auto report = vs::validate::validate(c.project.document, kSettings, &c.project.blobs);
  INFO(codes(report.diagnostics));
  CHECK_FALSE(report.diagnostics.has_errors());
}

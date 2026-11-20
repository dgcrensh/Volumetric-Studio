// L3: layout validation at every level of a nested document.

#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "nested.hpp"
#include "vs/model/build.hpp"
#include "vs/validate/layout.hpp"
#include "vs/validate/registry.hpp"
#include "vs/validate/validate.hpp"

using namespace vs::model;
using Catch::Matchers::WithinAbs;
using vs::core::Id;
using vs::feature::FeatureRef;
using vs::feature::Param;

namespace {

const auto kSettings = ProductSettings::defaults();

std::string codes(const Diagnostics& ds) {
  std::string s;
  for (const auto& d : ds) s += d.code + " ";
  return s;
}

const vs::core::Diagnostic* find(const Diagnostics& ds, const std::string& code) {
  for (const auto& d : ds) {
    if (d.code == code) return &d;
  }
  return nullptr;
}

vs::validate::LayoutReport layout_of(const Document& doc) {
  const auto b = build_layout(doc, kSettings);
  INFO(codes(b.diagnostics));
  REQUIRE(b.ok());
  return vs::validate::validate_layout(doc, b, kSettings);
}

Volume& vol(nested::Nested& n, const Id& id) { return *n.project.document.find_volume(id); }

// The datum point a box boundary's corner uses, in the volume's level.
vs::feature::PointAt& corner(nested::Nested& n, const Id& volume, bool max) {
  const Volume& v = vol(n, volume);
  const auto& box = std::get<BoxBoundary>(v.boundary);
  const Id point = std::get<FeatureRef>(max ? box.max : box.min).feature;
  vs::feature::FeatureTree* layout =
      v.parent.is_null() ? &n.project.document.layout : vol(n, v.parent).tree();
  auto* def = layout->find(point)->as<vs::feature::DatumPointFeature>();
  return std::get<vs::feature::PointAt>(def->definition);
}

}  // namespace

TEST_CASE("The nested document validates cleanly", "[validate][nested]") {
  const auto n = nested::make_nested();
  const auto r = layout_of(n.project.document);
  INFO(codes(r.diagnostics));
  CHECK(r.diagnostics.empty());
  CHECK(r.overlaps.empty());
  CHECK(r.contacts.empty());
}

TEST_CASE("Siblings inside a composite may not overlap", "[validate][nested]") {
  auto n = nested::make_nested();
  corner(n, n.cap, false).z = Param::of(5);  // Cap now starts 5 mm into Core
  const auto r = layout_of(n.project.document);
  INFO(codes(r.diagnostics));
  REQUIRE(r.overlaps.size() == 1);
  CHECK(((r.overlaps[0].a == n.core && r.overlaps[0].b == n.cap) ||
         (r.overlaps[0].a == n.cap && r.overlaps[0].b == n.core)));
  CHECK_THAT(r.overlaps[0].depth, WithinAbs(5.0, 1e-6));
  // Routed to Left's layout, where both corners are.
  const auto* d = find(r.diagnostics, "validate.overlap");
  REQUIRE(d);
  const auto t = vs::validate::route(*d, n.project.document);
  CHECK(t.on == vs::validate::ActOn::Layout);
  CHECK(t.volume == n.left);
}

TEST_CASE("A child reaching outside its composite is an error", "[validate][nested]") {
  auto n = nested::make_nested();
  corner(n, n.right, true).x = Param::expr("width + 5");
  const auto r = layout_of(n.project.document);
  INFO(codes(r.diagnostics));
  const auto* d = find(r.diagnostics, "validate.nesting.outside");
  REQUIRE(d);
  CHECK(d->severity == vs::core::Severity::Error);
  CHECK(d->subjects[0].id == n.right);
  CHECK(d->message.find("reaches 5 mm outside 'Housing'") != std::string::npos);
  const auto t = vs::validate::route(*d, n.project.document);
  CHECK(t.on == vs::validate::ActOn::Layout);
  CHECK(t.volume == n.housing);
  // Right isn't Base's sibling, so they aren't checked against each other.
  CHECK(r.overlaps.empty());
  CHECK(r.contacts.empty());
}

TEST_CASE("Contact is judged between siblings only", "[validate][nested]") {
  auto n = nested::make_nested();
  auto& surfaces = n.project.document.connections;
  surfaces.erase(surfaces.begin() + 2);  // Joint: Core and Cap now touch undeclared
  const auto r = layout_of(n.project.document);
  INFO(codes(r.diagnostics));
  REQUIRE(r.contacts.size() == 1);
  CHECK(((r.contacts[0].a == n.core && r.contacts[0].b == n.cap) ||
         (r.contacts[0].a == n.cap && r.contacts[0].b == n.core)));
}

TEST_CASE("A delegated surface must lie on the child that meets it", "[validate][nested]") {
  auto n = nested::make_nested();
  auto& left = std::get<CompositeContents>(vol(n, n.left).contents);
  left.delegations[0].child = n.cap;  // Seat is at z = 0; Cap starts at z = 10
  const auto r = layout_of(n.project.document);
  INFO(codes(r.diagnostics));
  const auto* d = find(r.diagnostics, "validate.connection.off_boundary");
  REQUIRE(d);
  CHECK(d->subjects[0].id == n.seat);
  CHECK(d->subjects[1].id == n.cap);
  CHECK(d->message.find("10 mm") != std::string::npos);
}

TEST_CASE("Whole nested documents validate, contents and all", "[validate][nested]") {
  auto n = nested::make_nested();
  nested::fill_leaves(n);
  const auto report = vs::validate::validate(n.project.document, kSettings);
  INFO(codes(report.diagnostics));
  CHECK_FALSE(report.diagnostics.has_errors());
  for (const auto& routed : report.routed) {
    CHECK(vs::validate::find_code(routed.diagnostic.code) != nullptr);
  }
}

// H2: bypasses of containment (rule 5) and of the layout and connection
// checks that used to pass. Each case failed before H2.

#include <atomic>
#include <cmath>
#include <functional>
#include <thread>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fixtures.hpp"
#include "vs/kernel/operations.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"
#include "vs/validate/containment.hpp"
#include "vs/validate/layout.hpp"
#include "vs/validate/registry.hpp"

using namespace vs::validate;
using namespace vs::model;
using vs::core::dbox3;
using vs::core::Diagnostic;
using vs::core::dvec3;
using vs::core::fvec3;
using vs::core::Id;
using vs::core::SubjectKind;
using vs::core::SubjectRef;
using vs::feature::DatumPlaneFeature;
using vs::feature::DatumPointFeature;
using vs::feature::FeatureRef;
using vs::feature::FeatureTree;
using vs::feature::Param;
using vs::feature::PlaneOffset;
using vs::feature::PointAt;
using vs::feature::PrincipalPlane;

namespace {

const auto kSettings = vs::core::ProductSettings::defaults();
const SubjectRef kVolume{SubjectKind::Volume, Id::random(), "Part"};

vs::kernel::Solid box(dvec3 lo, dvec3 hi) {
  auto b = vs::kernel::make_box(dbox3::from_min_max(lo, hi), kSettings);
  REQUIRE(b.ok());
  return *b;
}

LayoutReport layout_of(const Document& doc) {
  const auto errors = check(doc);
  INFO(fixtures::codes(errors));
  REQUIRE_FALSE(errors.has_errors());
  const auto b = build(doc, kSettings);
  INFO(fixtures::codes(b.diagnostics));
  REQUIRE(b.ok());
  return validate_layout(doc, b, kSettings);
}

std::size_t count(const vs::core::Diagnostics& ds, const std::string& code) {
  return ds.with_code(code).size();
}

}  // namespace

// --- containment ------------------------------------------------------------------

TEST_CASE("Curved contents bulging out of their boundary are caught", "[validate][containment][h2]") {
  const auto boundary = box({0, 0, 0}, {20, 20, 20});

  SECTION("a cylinder 0.4 mm out of one side") {
    // Every vertex of the part outside lies on the boundary (where the
    // circles meet x = 0): its farthest point is inside the curved face.
    auto cyl = vs::kernel::make_cylinder({{9.5, 10, 1}, {0, 0, 1}}, 9.9, 18, kSettings);
    REQUIRE(cyl);
    const auto r = check_containment(boundary, *cyl, kVolume, kSettings);
    CHECK_FALSE(r.contained);
    CHECK(count(r.diagnostics, "validate.containment") == 1);
    CHECK(r.reach > 0.3);
    CHECK(r.reach < 0.41);
  }
  SECTION("a sphere 0.5 mm out of one side") {
    // The part outside is a spherical cap whose one vertex (where the
    // sphere's seam meets x = 0) is on the boundary.
    auto sphere = vs::kernel::make_sphere({9, 10, 10}, 9.5, kSettings);
    REQUIRE(sphere);
    const auto r = check_containment(boundary, *sphere, kVolume, kSettings);
    CHECK_FALSE(r.contained);
    CHECK(count(r.diagnostics, "validate.containment") == 1);
    CHECK(r.reach > 0.45);
    CHECK(r.reach < 0.51);
  }
  SECTION("a cylinder flush with the sides is still inside") {
    auto cyl = vs::kernel::make_cylinder({{10, 10, 0}, {0, 0, 1}}, 10, 20, kSettings);
    REQUIRE(cyl);
    CHECK(check_containment(boundary, *cyl, kVolume, kSettings).contained);
  }
}

TEST_CASE("A broken mesh is undecided, never contained", "[validate][containment][h2]") {
  const auto boundary = box({0, 0, 0}, {20, 20, 20});
  std::vector<fvec3> positions{{1, 1, 1}, {19, 1, 1}, {1, 19, 1}, {1, 1, 19}};
  std::vector<std::uint32_t> indices{0, 2, 1, 0, 1, 3, 1, 2, 3, 0, 3, 2};
  REQUIRE(check_containment(boundary, MeshView{positions, indices}, kVolume, kSettings).contained);

  SECTION("a NaN vertex") {
    // NaN compares false with every plane, so the convex fast path used to
    // call it inside.
    positions[3].z = std::numeric_limits<float>::quiet_NaN();
    const auto r = check_containment(boundary, MeshView{positions, indices}, kVolume, kSettings);
    CHECK_FALSE(r.contained);
    CHECK(count(r.diagnostics, "validate.undecided") == 1);
  }
  SECTION("an infinite vertex") {
    positions[3].x = std::numeric_limits<float>::infinity();
    const auto r = check_containment(boundary, MeshView{positions, indices}, kVolume, kSettings);
    CHECK_FALSE(r.contained);
  }
  SECTION("a NaN centroid slack") {
    const std::vector<float> slack(4, std::numeric_limits<float>::quiet_NaN());
    MeshView view{positions, indices, slack};
    const auto r = check_containment(boundary, view, kVolume, kSettings);
    CHECK(count(r.diagnostics, "validate.undecided") == 1);
  }
  SECTION("triangles with no positions") {
    const std::vector<fvec3> none;
    const auto r = check_containment(boundary, MeshView{none, indices}, kVolume, kSettings);
    CHECK_FALSE(r.contained);
    CHECK(count(r.diagnostics, "validate.undecided") == 1);
  }
}

TEST_CASE("Mesh containment gives the same answer on any number of threads",
          "[validate][containment][h2]") {
  // A non-convex boundary (an L) so samples go to the classifier, in many
  // chunks, run on real threads, each with its own copy of the boundary.
  auto l = vs::kernel::unite(box({0, 0, 0}, {40, 10, 10}), box({0, 0, 0}, {10, 40, 10}));
  REQUIRE(l);
  std::vector<fvec3> positions;
  std::vector<std::uint32_t> indices;
  for (int i = 0; i < 120; ++i) {
    for (int j = 0; j < 120; ++j) {
      const float x = 0.5F + 0.35F * static_cast<float>(i);
      const float y = 0.5F + 0.35F * static_cast<float>(j);
      positions.push_back({x, y, 5.0F});
    }
  }
  for (std::uint32_t k = 0; k + 2 < positions.size(); k += 3) indices.insert(indices.end(), {k, k + 1, k + 2});
  const MeshView view{positions, indices};
  const auto serial = check_containment(*l, view, kVolume, kSettings);
  const ParallelFor threads = [](std::size_t n, const std::function<void(std::size_t)>& task) {
    std::vector<std::thread> pool;
    std::atomic<std::size_t> next{0};
    for (int t = 0; t < 4; ++t) {
      pool.emplace_back([&] {
        for (std::size_t k = next++; k < n; k = next++) task(k);
      });
    }
    for (auto& t : pool) t.join();
  };
  const auto parallel = check_containment(*l, view, kVolume, kSettings, threads);
  CHECK_FALSE(serial.contained);
  CHECK(serial.outside_points == parallel.outside_points);
  CHECK(serial.reach == parallel.reach);
  CHECK(serial.region == parallel.region);
}

// --- nesting (L3) -------------------------------------------------------------------

TEST_CASE("A child in a notch of a large composite is outside it", "[validate][nested][h2]") {
  // A 1,000 mm composite with a 9 mm cube cut from a corner by a keep-out:
  // 729 mm³ missing out of 10⁹, which the old "fills its box within the
  // tolerance times its volume" test (1,000 mm³ here) took for a full box.
  Document doc = make_document("Notch");
  auto point = [](FeatureTree& tree, dvec3 p, const std::string& name) {
    return tree.add(DatumPointFeature{PointAt{Param::of(p.x), Param::of(p.y), Param::of(p.z)}},
                    name);
  };
  Volume big;
  big.id = Id::random();
  big.name = "Big";
  big.boundary = BoxBoundary{FeatureRef{point(doc.layout, {0, 0, 0}, "Big low")},
                             FeatureRef{point(doc.layout, {1000, 1000, 1000}, "Big high")}};
  Volume notch;
  notch.id = Id::random();
  notch.name = "Notch";
  notch.keep_out = true;
  notch.boundary = BoxBoundary{FeatureRef{point(doc.layout, {991, 991, 991}, "Notch low")},
                               FeatureRef{point(doc.layout, {1000, 1000, 1000}, "Notch high")}};
  doc.keep_outs.push_back({Id::random(), "Corner", {notch.id}, {big.id}});

  CompositeContents contents;
  Volume child;
  child.id = Id::random();
  child.name = "Child";
  child.parent = big.id;
  child.boundary = BoxBoundary{FeatureRef{point(contents.layout, {992, 992, 992}, "Low")},
                               FeatureRef{point(contents.layout, {998, 998, 998}, "High")}};
  big.contents = std::move(contents);
  const Id child_id = child.id;
  doc.volumes = {std::move(big), std::move(notch), std::move(child)};

  const auto r = layout_of(doc);
  INFO(fixtures::codes(r.diagnostics));
  const auto found = r.diagnostics.with_code("validate.nesting.outside");
  REQUIRE(found.size() == 1);
  CHECK(found[0]->subjects.at(0).id == child_id);
}

// --- connection surfaces ----------------------------------------------------------

TEST_CASE("A patch must not float inside or poke through a volume it joins",
          "[validate][connection][h2]") {
  Document doc = make_document("t");
  const Id a = fixtures::add_box(doc, "A", {0, 0, 0}, {40, 20, 10});
  const Id b = fixtures::add_box(doc, "B", {0, 0, 10}, {40, 20, 20});

  SECTION("on the plane x = 20, through both") {
    const Id plane = doc.layout.add(
        DatumPlaneFeature{PlaneOffset{PrincipalPlane::YZ, Param::of(20)}}, "Across");
    ConnectionSurface c;
    c.id = Id::random();
    c.name = "Through";
    DatumPatch patch;  // u along y, v along z
    patch.plane = FeatureRef{plane};
    patch.u = Param::of(10);
    patch.v = Param::of(10);
    c.geometry = patch;
    c.volumes = {a, b};
    c.params = BondedParams{};
    doc.connections.push_back(c);
    const auto r = layout_of(doc);
    CHECK(fixtures::codes(r.diagnostics) ==
          "validate.connection.off_boundary validate.connection.off_boundary");
  }
  SECTION("on the faces they share, running past their sides, is fine") {
    fixtures::add_patch(doc, "Wide", {a, b}, 10, 20, 10, 60, 30, BondedParams{});
    CHECK(layout_of(doc).diagnostics.empty());
  }
}

TEST_CASE("A clearance surface doesn't excuse contact", "[validate][connection][h2]") {
  Document doc = make_document("t");
  const Id a = fixtures::add_box(doc, "A", {0, 0, 0}, {40, 20, 10});
  const Id b = fixtures::add_box(doc, "B", {0, 0, 10}, {40, 20, 20});  // touching A
  ClearanceParams gap;
  gap.gap = Param::of(1);
  fixtures::add_patch(doc, "Gap", {a, b}, 10, 20, 10, 40, 20, gap);
  const auto r = layout_of(doc);
  CHECK(fixtures::codes(r.diagnostics) == "validate.contact");
}

// --- routing ----------------------------------------------------------------------

TEST_CASE("validate.undecided goes where its first subject is", "[validate][registry][h2]") {
  Document doc = make_document("t");
  const Id a = fixtures::add_box(doc, "A", {0, 0, 0}, {40, 20, 10});
  const Id b = fixtures::add_box(doc, "B", {0, 0, 10}, {40, 20, 20});
  const Id s = fixtures::add_patch(doc, "S", {a, b}, 10, 20, 10, 40, 20, BondedParams{});

  const auto contents =
      route(Diagnostic::error("validate.undecided", "x", {doc.volume_subject(a)}), doc);
  CHECK(contents.on == ActOn::Contents);
  CHECK(contents.volume == a);

  const auto surface = route(Diagnostic::error("validate.undecided", "x",
                                               {doc.connection_subject(s), doc.boundary_subject(a)}),
                             doc);
  CHECK(surface.on == ActOn::ConnectionSurface);
  CHECK(surface.subject.id == s);

  const auto layout = route(Diagnostic::error("validate.undecided", "x",
                                              {doc.boundary_subject(a), doc.boundary_subject(b)}),
                            doc);
  CHECK(layout.on == ActOn::Layout);
}

TEST_CASE("Outputs and surfaces inside a composite route to its level", "[validate][registry][h2]") {
  Document doc = make_document("t");
  CompositeContents contents;
  Volume parent;
  parent.id = Id::random();
  parent.name = "Parent";
  Volume child;
  child.id = Id::random();
  child.name = "Child";
  child.parent = parent.id;
  const Id output = Id::random();
  child.outputs.push_back({output, "mass", OutputQuantity::Volume});
  ConnectionSurface c;
  c.id = Id::random();
  c.name = "Inside";
  c.volumes = {child.id};
  parent.contents = std::move(contents);
  const Id parent_id = parent.id;
  doc.volumes = {std::move(parent), std::move(child)};
  doc.connections.push_back(c);

  // Codes vs_validate doesn't own, routed to the layout by their subject.
  const auto o = route(Diagnostic::error("model.something", "x",
                                         {SubjectRef{SubjectKind::PublishedOutput, output, "o"}}),
                       doc);
  CHECK(o.on == ActOn::Layout);
  CHECK(o.volume == parent_id);
  const auto u = route(Diagnostic::error("validate.undecided", "x",
                                         {SubjectRef{SubjectKind::Link, Id::random(), "l"},
                                          doc.connection_subject(c.id)}),
                       doc);
  CHECK(u.on == ActOn::Layout);
  const auto lv = route(Diagnostic::error("validate.overlap", "x", {doc.connection_subject(c.id)}),
                        doc);
  CHECK(lv.on == ActOn::Layout);
  CHECK(lv.volume == parent_id);
}


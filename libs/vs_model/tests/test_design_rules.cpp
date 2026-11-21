// H2: bypasses of encapsulation, identity, one-way flow and build
// integrity that used to pass. Each case failed before H2.

#include <algorithm>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "bracket.hpp"
#include "nested.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"
#include "vs/model/dependency.hpp"
#include "vs/model/project.hpp"

using namespace vs::model;
using bracket::codes;
using vs::core::dvec2;
using vs::core::dvec3;
using vs::feature::DatumPointFeature;
using vs::feature::ExtrudeFeature;
using vs::feature::Extent;
using vs::feature::FaceRef;
using vs::feature::FeatureRef;
using vs::feature::Param;
using vs::feature::PointAt;
using vs::feature::PrincipalPlane;
using vs::feature::SketchFeature;
using vs::sketch::Sketch;

namespace {

const auto kSettings = ProductSettings::defaults();

bool has(const Diagnostics& ds, const std::string& code) { return !ds.with_code(code).empty(); }

Volume& vol(Document& doc, const Id& id) { return *doc.find_volume(id); }

[[maybe_unused]] CompositeContents& comp(Document& doc, const Id& id) {
  return std::get<CompositeContents>(vol(doc, id).contents);
}

// A closed rectangle sketch feature on XY.
Id rectangle_sketch(vs::feature::FeatureTree& tree, dvec2 lo, dvec2 hi, const std::string& name) {
  Sketch s(Id::random(), name);
  const Id a = s.add_point(lo);
  const Id b = s.add_point({hi.x, lo.y});
  const Id c = s.add_point(hi);
  const Id d = s.add_point({lo.x, hi.y});
  s.add_line(a, b);
  s.add_line(b, c);
  s.add_line(c, d);
  s.add_line(d, a);
  return tree.add(SketchFeature{s, PrincipalPlane::XY}, name);
}

}  // namespace

// --- identity ------------------------------------------------------------------------

TEST_CASE("Authored objects can't have the null id", "[model][check][h2]") {
  auto b = bracket::make_bracket();
  Document& doc = b.project.document;
  REQUIRE_FALSE(check(doc).has_errors());

  SECTION("a volume") {
    vol(doc, b.upright).id = Id::null();
    // (The link and the flange now name a missing volume too; the null id
    // is reported on its own.)
    CHECK(has(check(doc), "model.document.null_id"));
  }
  SECTION("a volume's output") {
    vol(doc, b.base).outputs[0].id = Id::null();
    CHECK(has(check(doc), "model.document.null_id"));
  }
  SECTION("a project file") {
    const std::string text = to_json(doc);
    REQUIRE(document_from_json(text));
    for (const Id& id : {b.upright, b.flange, b.base_mass, b.base_thickness, b.link,
                         b.base_block}) {
      // Every occurrence: the object's own id and any references to it.
      std::string broken = text;
      for (auto at = broken.find(id.to_string()); at != std::string::npos;
           at = broken.find(id.to_string(), at)) {
        broken.replace(at, 32, std::string(32, '0'));
      }
      REQUIRE(broken != text);
      INFO(id.to_string());
      CHECK_FALSE(document_from_json(broken));
    }
  }
}

TEST_CASE("Sketch constraints and sketch ids take part in id uniqueness", "[model][check][h2]") {
  auto b = bracket::make_bracket();
  Document& doc = b.project.document;
  auto* base = doc.layout.sketch(b.base_sketch);
  auto* upright = doc.layout.sketch(b.upright_sketch);
  REQUIRE(base != nullptr);
  REQUIRE(upright != nullptr);
  REQUIRE_FALSE(base->constraints().empty());
  REQUIRE_FALSE(upright->constraints().empty());
  // Give one of the upright's constraints the id of one of the base's.
  vs::sketch::Constraint copy = upright->constraints().front();
  const Id taken = base->constraints().front().id;
  upright->remove_constraint(copy.id);
  copy.id = taken;
  upright->add_constraint(copy);
  const auto found = check(doc);
  INFO(codes(found));
  CHECK(has(found, "model.document.duplicate_id"));
}

TEST_CASE("A composite can't be its own parent", "[model][check][h2]") {
  auto n = nested::make_nested();
  Document& doc = n.project.document;
  vol(doc, n.housing).parent = n.housing;
  const auto found = check(doc);
  INFO(codes(found));
  const auto parent = found.with_code("model.volume.parent");
  REQUIRE_FALSE(parent.empty());
  CHECK(parent[0]->subjects.at(0).id == n.housing);
}

// --- one-way data flow --------------------------------------------------------------

TEST_CASE("A through-all extrude depends on every body feature above it",
          "[model][dependency][h2]") {
  // Base's block (driven by base_h), the upright, then an unrelated blind
  // block, then a through-all extrude bounding a third volume. The
  // through-all reaches as far as the whole body, Base's block included,
  // so the third volume's output driving base_h is a loop, even with the
  // blind block between them.
  auto b = bracket::make_bracket();
  Document& doc = b.project.document;
  const Id spacer_sketch = rectangle_sketch(doc.layout, {100, 0}, {110, 10}, "Spacer outline");
  ExtrudeFeature spacer;
  spacer.sketch = spacer_sketch;
  spacer.depth = Param::of(5);
  doc.layout.add(spacer, "Spacer");
  const Id post_sketch = rectangle_sketch(doc.layout, {200, 0}, {210, 10}, "Post outline");
  ExtrudeFeature post;
  post.sketch = post_sketch;
  post.extent = Extent::ThroughAll;
  const Id post_id = doc.layout.add(post, "Post");

  Volume v;
  v.id = Id::random();
  v.name = "Post";
  v.boundary = FeatureBoundary{{post_id}};
  const Id out = Id::random();
  v.outputs.push_back({out, "volume", OutputQuantity::Volume, 1.0});
  const Id vid = v.id;
  doc.volumes.push_back(std::move(v));
  REQUIRE_FALSE(check(doc).has_errors());

  const auto g = DependencyGraph::build(doc);
  CHECK(g.depends_on(*g.find(NodeKind::Feature, post_id), *g.find(NodeKind::Feature, b.base_block)));
  CHECK(g.depends_on(*g.find(NodeKind::VolumeBoundary, vid), *g.find(NodeKind::Variable, b.base_h)));

  doc.links.push_back({Id::random(), {vid, out}, b.base_h});
  const auto found = check(doc);
  INFO(codes(found));
  CHECK(has(found, "model.dependency.loop"));
}

TEST_CASE("A blind extrude doesn't depend on unrelated body features", "[model][dependency][h2]") {
  auto b = bracket::make_bracket();
  const auto g = DependencyGraph::build(b.project.document);
  CHECK_FALSE(g.depends_on(*g.find(NodeKind::Feature, b.upright_block),
                           *g.find(NodeKind::Feature, b.base_block)));
}

TEST_CASE("Surfaces on a composite's boundary face order the waves", "[model][dependency][h2]") {
  // Housing's Middle surface placed on Housing's own +x face: Core (Middle
  // is passed down to it) then depends on Housing's boundary, so on w, so
  // on whatever drives w: here another volume's output, through a link.
  auto n = nested::make_nested();
  Document& doc = n.project.document;
  auto& middle = std::get<DatumPatch>(doc.find_connection(n.middle)->geometry);
  middle.plane = FaceRef{n.housing, "+x"};
  // A root volume of its own (Base's box uses w, so it can't drive w).
  Volume other;
  other.id = Id::random();
  other.name = "Other";
  other.boundary = BoxBoundary{
      FeatureRef{doc.layout.add(DatumPointFeature{PointAt{Param::of(100), Param::of(0), Param::of(0)}})},
      FeatureRef{doc.layout.add(DatumPointFeature{PointAt{Param::of(110), Param::of(10), Param::of(10)}})}};
  const Id out = Id::random();
  other.outputs.push_back({out, "volume", OutputQuantity::Volume, 1.0});
  const Id other_id = other.id;
  doc.volumes.push_back(std::move(other));
  doc.links.push_back({Id::random(), {other_id, out}, n.w});
  REQUIRE_FALSE(DependencyGraph::build(doc).check().has_errors());

  const auto g = DependencyGraph::build(doc);
  const auto middle_node = g.find(NodeKind::ConnectionSurface, n.middle);
  const auto core = g.find(NodeKind::VolumeContents, n.core);
  REQUIRE(middle_node);
  REQUIRE(core);
  CHECK(g.depends_on(*middle_node, *g.find(NodeKind::VolumeBoundary, n.housing)));
  CHECK(g.depends_on(*core, *g.find(NodeKind::VolumeContents, other_id)));
  const auto order = g.order();
  const auto pos = [&](std::size_t k) { return std::find(order.begin(), order.end(), k) - order.begin(); };
  CHECK(pos(*g.find(NodeKind::VolumeContents, other_id)) < pos(*core));
}

// --- build integrity -----------------------------------------------------------------

TEST_CASE("A failed composite keeps its layout's rebuild", "[model][build][h2]") {
  auto n = nested::make_nested();
  Document& doc = n.project.document;
  // Housing's width parameter fails, so its layout (Right's far corner uses
  // it) doesn't rebuild cleanly and Housing fails.
  vol(doc, n.housing).parameters[0].value = Param::expr("no_such_variable");
  const auto built = build(doc, kSettings);
  const VolumeBuild* housing = built.find_volume(n.housing);
  REQUIRE(housing != nullptr);
  CHECK(housing->status == VolumeStatus::Failed);
  REQUIRE(housing->contents_rebuild.has_value());
  CHECK(housing->contents_rebuild->find(n.housing_split) != nullptr);
}

TEST_CASE("A keep-out whose cutter failed fails the boundary it cuts", "[model][build][h2]") {
  Document doc = make_document("Cutters");
  auto point = [&](dvec3 p, const std::string& name) {
    return doc.layout.add(DatumPointFeature{PointAt{Param::of(p.x), Param::of(p.y), Param::of(p.z)}},
                          name);
  };
  Volume block;
  block.id = Id::random();
  block.name = "Block";
  block.boundary = BoxBoundary{FeatureRef{point({0, 0, 0}, "lo")}, FeatureRef{point({20, 20, 20}, "hi")}};
  Volume hole;
  hole.id = Id::random();
  hole.name = "Hole";
  hole.keep_out = true;
  const Id corner = point({5, 5, 5}, "corner");
  hole.boundary = BoxBoundary{FeatureRef{corner}, FeatureRef{corner}};  // no volume: fails
  doc.keep_outs.push_back({Id::random(), "Bore", {hole.id}, {block.id}});
  const Id block_id = block.id;
  doc.volumes = {std::move(block), std::move(hole)};

  const auto built = build(doc, kSettings);
  INFO(codes(built.diagnostics));
  const VolumeBuild* b = built.find_volume(block_id);
  REQUIRE(b != nullptr);
  CHECK(b->boundary.is_empty());
  CHECK(b->status == VolumeStatus::Failed);
  CHECK(has(b->diagnostics, "model.boundary.feature_failed"));
}

TEST_CASE("A failed child's partial contents don't count in its composite's outputs",
          "[model][build][h2]") {
  auto n = nested::make_nested();
  nested::fill_leaves(n);
  Document& doc = n.project.document;
  const Id total = Id::random();
  vol(doc, n.housing).outputs.push_back({total, "volume", OutputQuantity::Volume, 1.0});
  {
    const auto built = build(doc, kSettings);
    REQUIRE(built.find_volume(n.housing)->status == VolumeStatus::Current);
    const auto& o = built.find_volume(n.housing)->outputs.at(0);
    REQUIRE(o.value);
    CHECK(std::get<double>(*o.value) == Catch::Approx(16000));  // Core, Cap, Right
  }
  // Core's tree gets a feature that fails after its block is made.
  auto& tree = std::get<ParametricContents>(vol(doc, n.core).contents).tree;
  ExtrudeFeature broken;
  broken.sketch = Id::random();
  tree.add(broken, "Broken");
  const auto built = build(doc, kSettings);
  REQUIRE(built.find_volume(n.core)->status == VolumeStatus::Failed);
  REQUIRE_FALSE(built.find_volume(n.core)->contents.is_empty());
  const auto& o = built.find_volume(n.housing)->outputs.at(0);
  REQUIRE(o.value);
  CHECK(std::get<double>(*o.value) == Catch::Approx(12000));  // Cap and Right
}

// L3: composite volumes and deep nesting in the model.

#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "bracket.hpp"
#include "nested.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"
#include "vs/model/dependency.hpp"
#include "vs/model/hierarchy.hpp"
#include "vs/model/project.hpp"
#include "vs/model/references.hpp"

using namespace vs::model;
using bracket::codes;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using vs::core::dvec3;
using vs::core::Id;
using vs::feature::FeatureRef;
using vs::feature::Param;

namespace {

const auto kSettings = ProductSettings::defaults();

std::string messages(const Diagnostics& ds) {
  std::string out;
  for (const auto& d : ds) out += d.code + ": " + d.message + "\n";
  return out;
}

bool has_code(const Diagnostics& ds, const std::string& code) {
  for (const auto& d : ds) {
    if (d.code == code) return true;
  }
  return false;
}

Volume& vol(nested::Nested& n, const Id& id) { return *n.project.document.find_volume(id); }

CompositeContents& comp(nested::Nested& n, const Id& id) {
  return std::get<CompositeContents>(vol(n, id).contents);
}

void check_box(const vs::kernel::Solid& s, const vs::core::dbox3& expected) {
  const auto b = vs::kernel::bounding_box(s);
  CHECK_THAT(b.min.x, WithinAbs(expected.min.x, 1e-6));
  CHECK_THAT(b.min.y, WithinAbs(expected.min.y, 1e-6));
  CHECK_THAT(b.min.z, WithinAbs(expected.min.z, 1e-6));
  CHECK_THAT(b.max.x, WithinAbs(expected.max.x, 1e-6));
  CHECK_THAT(b.max.y, WithinAbs(expected.max.y, 1e-6));
  CHECK_THAT(b.max.z, WithinAbs(expected.max.z, 1e-6));
}

}  // namespace

TEST_CASE("A nested document is valid and round-trips through JSON exactly", "[model][nested]") {
  auto n = nested::make_nested();
  const Document& doc = n.project.document;
  const auto findings = check(doc);
  INFO(codes(findings));
  CHECK(findings.empty());

  const std::string json = to_json(doc);
  auto back = document_from_json(json);
  REQUIRE(back);
  CHECK(*back == doc);
  CHECK(to_json(*back) == json);
  CHECK(content_hash(*back) == content_hash(doc));

  // Only children carry a parent, so flat documents keep their JSON.
  CHECK(json.find("\"parent\"") != std::string::npos);
  CHECK(to_json(bracket::make_bracket().project.document).find("\"parent\"") == std::string::npos);
  CHECK(vol(n, n.housing).kind() == VolumeKind::Composite);
  CHECK(to_string(VolumeKind::Composite) == "composite");
}

TEST_CASE("The tree of volumes: levels, paths and delegated surfaces", "[model][nested]") {
  const auto n = nested::make_nested();
  const Document& doc = n.project.document;

  CHECK(children(doc, {}).size() == 2);
  CHECK(children(doc, n.housing).size() == 2);
  CHECK(children(doc, n.left).size() == 2);
  CHECK(children(doc, n.core).empty());
  CHECK(depth(doc, n.core) == 2);
  CHECK(ancestors(doc, n.cap) == std::vector<Id>{n.left, n.housing});
  CHECK(child_toward(doc, {}, n.cap) == n.housing);
  CHECK(child_toward(doc, n.housing, n.cap) == n.left);
  CHECK(is_within(doc, n.cap, n.housing));
  CHECK_FALSE(is_within(doc, n.right, n.left));
  CHECK(volume_path(doc, n.core) == "Housing / Left / Core");
  CHECK(level_layout(doc, n.left) == doc.find_volume(n.left)->tree());
  CHECK(level_layout(doc, n.core) == nullptr);

  // Parents before children.
  const auto order = level_order(doc);
  REQUIRE(order.size() == doc.volumes.size());
  for (std::size_t k = 0; k < order.size(); ++k) {
    for (std::size_t j = k + 1; j < order.size(); ++j) {
      CHECK_FALSE(is_within(doc, doc.volumes[order[k]].id, doc.volumes[order[j]].id));
    }
  }

  const ConnectionSurface& seat = *doc.find_connection(n.seat);
  const ConnectionSurface& middle = *doc.find_connection(n.middle);
  CHECK(level_of(doc, seat).is_null());
  CHECK(level_of(doc, middle) == n.housing);
  CHECK(joins(doc, seat, n.base));
  CHECK(joins(doc, seat, n.housing));
  CHECK(joins(doc, seat, n.left));
  CHECK(joins(doc, seat, n.core));
  CHECK_FALSE(joins(doc, seat, n.cap));
  CHECK_FALSE(joins(doc, seat, n.right));
  CHECK(joins(doc, middle, n.core));
  CHECK_FALSE(joins(doc, middle, n.cap));
  CHECK(seam_sides(doc, seat) == std::vector<Id>{n.base, n.core});
  CHECK(seam_sides(doc, middle) == std::vector<Id>{n.core, n.right});
  CHECK(joining_surfaces(doc, n.core).size() == 3);
  CHECK(joining_surfaces(doc, n.cap).size() == 1);
}

TEST_CASE("Children are built from their composite's layout", "[model][nested]") {
  auto n = nested::make_nested();
  nested::fill_leaves(n);
  for (const Id& c : {n.housing, n.left}) {
    vol(n, c).outputs.push_back({Id::random(), "volume", OutputQuantity::Volume, 1.0});
    vol(n, c).outputs.push_back({Id::random(), "mass", OutputQuantity::Mass, 2.0});
    vol(n, c).outputs.push_back({Id::random(), "centre", OutputQuantity::CentreOfGravity, 1.0});
    vol(n, c).outputs.push_back({Id::random(), "envelope", OutputQuantity::Envelope, 1.0});
  }
  REQUIRE_FALSE(check(n.project.document).has_errors());

  const auto r = build(n.project.document, kSettings);
  INFO(codes(r.diagnostics));
  REQUIRE(r.ok());
  for (const auto& v : n.project.document.volumes) {
    INFO(v.name);
    const VolumeBuild* vb = r.find_volume(v.id);
    REQUIRE(vb);
    check_box(vb->boundary, nested::box_of(n, v.id));
    CHECK(vb->status == VolumeStatus::Current);
    CHECK(vb->contents.is_empty() == v.composite());
    CHECK(vb->contents_rebuild.has_value());
  }
  for (const Id& s : {n.seat, n.middle, n.joint}) CHECK(r.find_connection(s)->ok);

  // Outputs add the children up: Housing is Left (Core + Cap) and Right.
  const VolumeBuild& housing = *r.find_volume(n.housing);
  REQUIRE(housing.outputs.size() == 4);
  CHECK_THAT(std::get<double>(*housing.outputs[0].value), WithinRel(40.0 * 20 * 20, 1e-9));
  CHECK_THAT(std::get<double>(*housing.outputs[1].value), WithinRel(16.0 * 2.0, 1e-9));
  const auto centre = std::get<dvec3>(*housing.outputs[2].value);
  CHECK_THAT(centre.x, WithinAbs(20.0, 1e-6));
  CHECK_THAT(centre.y, WithinAbs(10.0, 1e-6));
  CHECK_THAT(centre.z, WithinAbs(10.0, 1e-6));
  const auto envelope = std::get<vs::core::dbox3>(*housing.outputs[3].value);
  CHECK_THAT(envelope.max.x, WithinAbs(40.0, 1e-6));
  const VolumeBuild& left = *r.find_volume(n.left);
  CHECK_THAT(std::get<double>(*left.outputs[0].value), WithinRel(20.0 * 20 * 20, 1e-9));

  // A root variable reaches the grandchildren through Housing's parameter.
  REQUIRE_FALSE(n.project.document.layout.variables().set_expression(n.w, "50").has_errors());
  const auto wider = build_layout(n.project.document, kSettings);
  INFO(codes(wider.diagnostics));
  REQUIRE(wider.ok());
  check_box(wider.find_volume(n.right)->boundary,
            vs::core::dbox3::from_min_max({20, 0, 0}, {50, 20, 20}));
  CHECK(wider.find_volume(n.housing)->parameters.at("width") == 50.0);
}

TEST_CASE("A child's boundary can anchor to its composite's inputs", "[model][nested]") {
  // Right's far corner as a face of Housing's boundary: its "+x" face.
  auto n = nested::make_nested();
  auto& hl = comp(n, n.housing).layout;
  const Id far = hl.add(vs::feature::DatumPointFeature{vs::feature::PointAxisPlane{
                            vs::feature::PrincipalAxis::X,
                            vs::feature::FaceRef{n.housing, "+x"}}},
                        "Far");
  (void)far;
  REQUIRE_FALSE(check(n.project.document).has_errors());
  const auto r = build_layout(n.project.document, kSettings);
  INFO(codes(r.diagnostics));
  CHECK(r.ok());
}

TEST_CASE("Encapsulation holds at every level", "[model][nested]") {
  SECTION("the root can't see into a composite's layout") {
    auto n = nested::make_nested();
    std::get<BoxBoundary>(vol(n, n.base).boundary).max = FeatureRef{n.housing_split};
    CHECK(has_code(check(n.project.document), "model.reference.contents"));
  }
  SECTION("the root can't see a grandchild") {
    auto n = nested::make_nested();
    n.project.document.connections[0].volumes = {n.base, n.core};
    CHECK(has_code(check(n.project.document), "model.level.mixed"));
  }
  SECTION("a child can't see its composite's parent layout") {
    auto n = nested::make_nested();
    std::get<BoxBoundary>(vol(n, n.core).boundary).max = FeatureRef{n.housing_split};
    CHECK(has_code(check(n.project.document), "model.reference.parent"));
  }
  SECTION("a composite can't see inside its children") {
    auto n = nested::make_nested();
    std::get<BoxBoundary>(vol(n, n.right).boundary).max = FeatureRef{n.left_mid};
    CHECK(has_code(check(n.project.document), "model.reference.contents"));
  }
  SECTION("a child's parameter uses its composite's names, not the root's") {
    auto n = nested::make_nested();
    vol(n, n.right).parameters.push_back({Id::random(), "p", Param::expr("width / 2")});
    CHECK_FALSE(check(n.project.document).has_errors());
    vol(n, n.right).parameters.back().value = Param::expr("w / 2");
    CHECK(has_code(check(n.project.document), "model.reference.parent"));
  }
  SECTION("links stay inside a level") {
    auto n = nested::make_nested();
    const Id out = Id::random();
    vol(n, n.core).outputs.push_back({out, "volume", OutputQuantity::Volume, 1.0});
    const Id p = Id::random();
    vol(n, n.cap).parameters.push_back({p, "p", Param::of(1)});
    n.project.document.links.push_back({Id::random(), {n.core, out}, p});
    INFO(messages(check(n.project.document)));
    CHECK_FALSE(check(n.project.document).has_errors());

    const Id q = Id::random();
    vol(n, n.right).parameters.push_back({q, "q", Param::of(1)});
    n.project.document.links.back().target = q;  // up a level: Right is Left's sibling
    CHECK(check(n.project.document).has_errors());

    const Id r = Id::random();
    vol(n, n.base).outputs.push_back({r, "volume", OutputQuantity::Volume, 1.0});
    n.project.document.links.back() = {n.project.document.links.back().id, {n.base, r}, p};
    CHECK(has_code(check(n.project.document), "model.reference.contents"));

    // A composite's own parameter is driven from outside it.
    n.project.document.links.back() = {n.project.document.links.back().id, {n.core, out},
                                       *&vol(n, n.housing).parameters[0].id};
    CHECK(check(n.project.document).has_errors());
  }
}

TEST_CASE("Parents, delegations and composite outputs are checked", "[model][nested]") {
  SECTION("only a composite has children") {
    auto n = nested::make_nested();
    vol(n, n.cap).parent = n.right;
    CHECK(has_code(check(n.project.document), "model.volume.parent"));
  }
  SECTION("parents don't loop") {
    auto n = nested::make_nested();
    vol(n, n.housing).parent = n.left;
    CHECK(has_code(check(n.project.document), "model.volume.parent"));
  }
  SECTION("a missing parent") {
    auto n = nested::make_nested();
    vol(n, n.right).parent = Id::random();
    CHECK(has_code(check(n.project.document), "model.volume.parent"));
  }
  SECTION("a delegation names a joining surface and a child") {
    auto n = nested::make_nested();
    comp(n, n.housing).delegations.push_back({n.joint, n.right});
    CHECK(has_code(check(n.project.document), "model.composite.delegation"));
    comp(n, n.housing).delegations.back() = {n.seat, n.right};
    CHECK(has_code(check(n.project.document), "model.composite.delegation"));  // twice
    comp(n, n.housing).delegations.pop_back();
    comp(n, n.housing).delegations[0].child = n.core;  // a grandchild
    CHECK(has_code(check(n.project.document), "model.composite.delegation"));
  }
  SECTION("an undelegated surface warns") {
    auto n = nested::make_nested();
    comp(n, n.left).delegations.clear();
    const auto findings = check(n.project.document);
    INFO(codes(findings));
    CHECK_FALSE(findings.has_errors());
    CHECK(has_code(findings, "model.composite.undelegated"));
    CHECK_FALSE(joins(n.project.document, *n.project.document.find_connection(n.seat), n.core));
  }
  SECTION("composite outputs add up") {
    auto n = nested::make_nested();
    vol(n, n.housing).outputs.push_back({Id::random(), "area", OutputQuantity::SurfaceArea, 1.0});
    CHECK(has_code(check(n.project.document), "model.volume.bad_output"));
  }
  SECTION("a keep-out can't be a composite") {
    auto n = nested::make_nested();
    vol(n, n.left).keep_out = true;
    CHECK(has_code(check(n.project.document), "model.volume.keep_out_contents"));
  }
}

TEST_CASE("Dependency loops through a composite's layout are found", "[model][nested]") {
  auto n = nested::make_nested();
  auto& hl = comp(n, n.housing).layout;
  const Id rx = *hl.variables().add("rx", "20");
  // Right's low corner at x = rx.
  for (const auto& f : hl.features()) {
    if (hl.label(f.id) == "Right low") {
      auto* def = hl.find(f.id)->as<vs::feature::DatumPointFeature>();
      std::get<vs::feature::PointAt>(def->definition).x = Param::expr("rx");
      break;
    }
  }
  const Id out = Id::random();
  vol(n, n.right).outputs.push_back({out, "volume", OutputQuantity::Volume, 1.0});
  REQUIRE_FALSE(check(n.project.document).has_errors());

  const auto graph = DependencyGraph::build(n.project.document);
  const auto contents = graph.find(NodeKind::VolumeContents, n.housing);
  const auto core = graph.find(NodeKind::VolumeContents, n.core);
  REQUIRE(contents);
  REQUIRE(core);
  CHECK(graph.depends_on(*contents, *core));
  CHECK(graph.depends_on(*graph.find(NodeKind::VolumeBoundary, n.right),
                         *graph.find(NodeKind::Variable, rx)));

  // Right's volume driving rx, which places Right: a loop.
  n.project.document.links.push_back({Id::random(), {n.right, out}, rx});
  const auto findings = check(n.project.document);
  INFO(codes(findings));
  CHECK(has_code(findings, "model.dependency.loop"));
  // Driving it from Core's volume instead is fine.
  vol(n, n.core).outputs.push_back({Id::random(), "v", OutputQuantity::Volume, 1.0});
  n.project.document.links.back().source = {n.left, Id::random()};
  vol(n, n.left).outputs.push_back({n.project.document.links.back().source.output, "v",
                                    OutputQuantity::Volume, 1.0});
  INFO(messages(check(n.project.document)));
  CHECK_FALSE(check(n.project.document).has_errors());
}

TEST_CASE("Diagnostics at a composite's level name the composite", "[model][nested]") {
  auto n = nested::make_nested();
  vol(n, n.right).parameters.push_back({Id::random(), "p", Param::expr("nope + 1")});
  const auto r = build_layout(n.project.document, kSettings);
  CHECK(r.find_volume(n.right)->status == VolumeStatus::Failed);
  // A failed composite layout fails its children's boundaries.
  auto m = nested::make_nested();
  auto& ll = comp(m, m.left).layout;
  for (const auto& f : ll.features()) {
    if (ll.label(f.id) == "Middle") {
      REQUIRE(ll.set_suppressed(f.id, true));
      break;
    }
  }
  const auto broken = build_layout(m.project.document, kSettings);
  CHECK(broken.find_volume(m.core)->boundary.is_empty());
  CHECK(has_code(broken.find_volume(m.core)->diagnostics, "model.boundary.point_failed"));
  CHECK_FALSE(broken.find_volume(m.cap)->boundary.is_empty());
}

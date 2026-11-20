// L2: curved (cylindrical) connection surfaces in the model.

#include <numbers>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "bracket.hpp"
#include "curved.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"
#include "vs/model/dependency.hpp"
#include "vs/model/project.hpp"

using namespace vs::model;
using bracket::codes;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using vs::feature::FeatureRef;
using vs::feature::Param;

namespace {

const auto kSettings = ProductSettings::defaults();
constexpr double kPi = std::numbers::pi;

bool has_code(const Diagnostics& ds, const std::string& code) {
  for (const auto& d : ds) {
    if (d.code == code) return true;
  }
  return false;
}

CylinderPatch& patch_of(Document& d) { return std::get<CylinderPatch>(d.connections[0].geometry); }

}  // namespace

TEST_CASE("A band round the shaft builds as a cylinder face", "[model][curved]") {
  const auto c = curved::make_curved();
  REQUIRE_FALSE(check(c.project.document).has_errors());
  const auto r = build(c.project.document, kSettings, &c.project.blobs);
  INFO(codes(r.diagnostics));
  REQUIRE(r.ok());
  const ConnectionBuild* cb = r.find_connection(c.surface);
  REQUIRE(cb);
  REQUIRE(cb->ok);
  REQUIRE(cb->cylinder);
  CHECK(cb->cylinder->band());
  CHECK(cb->cylinder->radius == 10.0);
  CHECK_THAT(cb->area, WithinRel(2 * kPi * 10 * 30, 1e-12));
  CHECK_THAT(vs::kernel::surface_area(cb->patch), WithinRel(2 * kPi * 10 * 30, 1e-6));
  const auto cyl = vs::kernel::face_cylinder(cb->patch);
  REQUIRE(cyl);
  CHECK_THAT(cyl->radius, WithinAbs(10.0, 1e-12));
  CHECK_FALSE(cyl->inward);
  // The frame: on the z axis, angle 0 along x.
  CHECK(cb->frame.origin == vs::core::dvec3{0, 0, 0});
  CHECK(cb->frame.normal() == vs::core::dvec3{0, 0, 1});
  CHECK(cb->frame.x_axis == vs::core::dvec3{1, 0, 0});
}

TEST_CASE("A half-turn patch builds as half the cylinder", "[model][curved]") {
  curved::Options o;
  o.half = true;
  const auto c = curved::make_curved(o);
  const auto r = build(c.project.document, kSettings, &c.project.blobs);
  INFO(codes(r.diagnostics));
  REQUIRE(r.ok());
  const ConnectionBuild* cb = r.find_connection(c.surface);
  REQUIRE(cb->cylinder);
  CHECK_FALSE(cb->cylinder->band());
  CHECK_THAT(vs::kernel::surface_area(cb->patch), WithinRel(kPi * 10 * 30, 1e-6));
  // Its points have y >= 0.
  const auto box = vs::kernel::bounding_box(cb->patch);
  CHECK(box.min.y > -1e-6);
}

TEST_CASE("Cylinder frames depend on the axis alone", "[model][curved]") {
  for (const vs::core::dvec3 d : {vs::core::dvec3{0, 0, 1}, vs::core::dvec3{1, 0, 0},
                                  vs::core::dvec3{0.3, -0.4, 0.866}, vs::core::dvec3{0, 0, -1}}) {
    const auto f = cylinder_frame({{5, 6, 7}, d});
    CHECK(f.origin == vs::core::dvec3{5, 6, 7});
    CHECK_THAT(vs::core::length(f.x_axis), WithinAbs(1.0, 1e-12));
    CHECK_THAT(vs::core::dot(f.x_axis, d), WithinAbs(0.0, 1e-12));
    CHECK_THAT(vs::core::dot(f.normal(), vs::core::normalize(d)), WithinAbs(1.0, 1e-12));
  }
}

TEST_CASE("Cylinder patches round-trip through JSON and project files", "[model][curved][project]") {
  auto c = curved::make_curved();
  auto& p = patch_of(c.project.document);
  p.radius = Param::expr("r");
  p.sweep = Param::of(1.5);
  p.start_angle = Param::of(0.25);
  REQUIRE(c.project.document.layout.variables().add("r", "10 mm").ok());
  c.project.document.connections[0].seam.spacing = 0.75;
  const std::string text = to_json(c.project.document);
  CHECK(text.find("\"cylinder\"") != std::string::npos);
  auto back = document_from_json(text);
  REQUIRE(back.ok());
  CHECK(*back == c.project.document);
  CHECK(to_json(*back) == text);
  CHECK(content_hash(*back) == content_hash(c.project.document));

  const auto path = std::filesystem::temp_directory_path() /
                    ("vs_curved_" + vs::core::Id::random().to_string() + ".vsproj");
  REQUIRE_FALSE(save_project(c.project, path, ProjectLayout::Zip).has_errors());
  auto loaded = load_project(path);
  std::filesystem::remove(path);
  REQUIRE(loaded.ok());
  CHECK(content_hash(*loaded) == content_hash(c.project));
}

TEST_CASE("A cylinder patch follows its radius expression and its axis feature",
          "[model][curved]") {
  auto c = curved::make_curved();
  Document& doc = c.project.document;
  const Id axis = doc.layout.add(
      vs::feature::DatumAxisFeature{vs::feature::AxisAlong{vs::feature::PrincipalAxis::Z}}, "Axis");
  REQUIRE(doc.layout.variables().add("bore_r", "10 mm").ok());
  auto& p = patch_of(doc);
  p.axis = FeatureRef{axis};
  p.radius = Param::expr("bore_r");
  REQUIRE_FALSE(check(doc).has_errors());
  const auto r = build(doc, kSettings);
  INFO(codes(r.diagnostics));
  REQUIRE(r.find_connection(c.surface)->ok);
  CHECK(r.find_connection(c.surface)->cylinder->radius == 10.0);

  // An axis that is gone fails the build of the surface.
  Document broken = doc;
  patch_of(broken).axis = FeatureRef{Id::random()};
  const auto rb = build(broken, kSettings);
  CHECK(has_code(rb.diagnostics, "model.connection.axis_failed"));
}

TEST_CASE("Bad cylinder patches are refused with codes", "[model][curved]") {
  SECTION("a curved bolted flange") {
    curved::Options o;
    o.params = BoltedFlangeParams{};
    const auto c = curved::make_curved(o);
    const auto ds = check(c.project.document);
    INFO(codes(ds));
    CHECK(has_code(ds, "model.connection.curved_flange"));
    const auto r = build(c.project.document, kSettings);
    CHECK(has_code(r.diagnostics, "model.connection.curved_flange"));
  }
  SECTION("a sweep over a full turn") {
    auto c = curved::make_curved();
    patch_of(c.project.document).sweep = Param::of(7.0);
    const auto r = build(c.project.document, kSettings);
    CHECK(has_code(r.diagnostics, "model.connection.bad_size"));
    CHECK_FALSE(r.find_connection(c.surface)->ok);
  }
  SECTION("no radius") {
    auto c = curved::make_curved();
    patch_of(c.project.document).radius = Param::of(0.0);
    const auto r = build(c.project.document, kSettings);
    CHECK(has_code(r.diagnostics, "model.connection.bad_size"));
  }
  SECTION("an axis that isn't one") {
    auto c = curved::make_curved();
    patch_of(c.project.document).axis = FeatureRef{c.shaft_block};
    const auto r = build(c.project.document, kSettings);
    CHECK(has_code(r.diagnostics, "model.connection.axis_failed"));
  }
}

TEST_CASE("Contents inside the shaft see the band as an axis and a cylinder",
          "[model][curved]") {
  auto c = curved::make_curved();
  Document& doc = c.project.document;
  // The shaft's contents: a rectangle on the ZX plane revolved about the
  // band's axis, filling the shaft.
  ParametricContents contents;
  vs::sketch::Sketch s(Id::random(), "Section");
  const Id a = s.add_point({0, 0});
  const Id b = s.add_point({30, 0});  // ZX: x is along z, y along x
  const Id cc = s.add_point({30, 10});
  const Id d = s.add_point({0, 10});
  s.add_line(a, b);
  s.add_line(b, cc);
  s.add_line(cc, d);
  s.add_line(d, a);
  const Id sketch = contents.tree.add(
      vs::feature::SketchFeature{s, vs::feature::PrincipalPlane::ZX});
  vs::feature::RevolveFeature rev;
  rev.sketch = sketch;
  rev.axis = FeatureRef{c.surface};
  contents.tree.add(rev, "Turned");
  doc.find_volume(c.shaft)->contents = std::move(contents);
  const auto ds = check(doc);
  INFO(codes(ds));
  REQUIRE_FALSE(ds.has_errors());
  const auto r = build(doc, kSettings);
  INFO(codes(r.diagnostics));
  const VolumeBuild* vb = r.find_volume(c.shaft);
  REQUIRE(vb->status == VolumeStatus::Current);
  const auto m = vs::kernel::mass_properties(vb->contents);
  REQUIRE(m.ok());
  CHECK_THAT(m->volume, WithinRel(kPi * 100 * 30, 1e-6));
}

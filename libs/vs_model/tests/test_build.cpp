#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "bracket.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"

using namespace vs::model;
using bracket::codes;
using Catch::Approx;
using vs::feature::FeatureRef;
using vs::feature::PointAt;

namespace {

const auto kSettings = ProductSettings::defaults();

double volume_of(const vs::kernel::Solid& s) {
  const auto m = vs::kernel::mass_properties(s);
  REQUIRE(m.ok());
  return m->volume;
}

const OutputValue* value_of(const VolumeBuild& vb, const Id& output) {
  for (const auto& o : vb.outputs) {
    if (o.output == output && o.value) return &*o.value;
  }
  return nullptr;
}

// A keep-out volume spanning two datum points added to the layout.
Id add_box_volume(Document& d, const std::string& name, vs::core::dvec3 lo, vs::core::dvec3 hi,
                  bool keep_out) {
  const Id a = d.layout.add(vs::feature::DatumPointFeature{PointAt{
                                Param::of(lo.x), Param::of(lo.y), Param::of(lo.z)}},
                            name + " lo");
  const Id b = d.layout.add(vs::feature::DatumPointFeature{PointAt{
                                Param::of(hi.x), Param::of(hi.y), Param::of(hi.z)}},
                            name + " hi");
  Volume v;
  v.id = Id::random();
  v.name = name;
  v.keep_out = keep_out;
  v.boundary = BoxBoundary{FeatureRef{a}, FeatureRef{b}};
  d.volumes.push_back(v);
  return v.id;
}

}  // namespace

TEST_CASE("The bracket builds: boundaries, flange and contents", "[model][build]") {
  const auto b = bracket::make_bracket();
  const auto r = build(b.project.document, kSettings, &b.project.blobs);
  INFO(codes(r.diagnostics));
  REQUIRE(r.ok());

  const VolumeBuild* base = r.find_volume(b.base);
  const VolumeBuild* upright = r.find_volume(b.upright);
  REQUIRE(base);
  REQUIRE(upright);
  CHECK(volume_of(base->boundary) == Approx(60.0 * 40 * 10));
  CHECK(volume_of(upright->boundary) == Approx(60.0 * 10 * 40));

  // Base's contents fill its boundary, `thickness` (= base_h) deep.
  CHECK(base->status == VolumeStatus::Current);
  CHECK(base->parameters.at("thickness") == Approx(10));
  REQUIRE(base->contents_rebuild);
  CHECK(volume_of(base->contents) == Approx(24000));
  const auto* mass = value_of(*base, b.base_mass);
  REQUIRE(mass);
  CHECK(std::get<double>(*mass) == Approx(24.0 * 1.24));  // 24 cm³ of PLA
  CHECK(std::get<double>(*value_of(*base, b.base_volume)) == Approx(24000));

  // Upright's contents come from its STEP file.
  CHECK(upright->status == VolumeStatus::Current);
  CHECK(volume_of(upright->contents) == Approx(24000));
  const auto* env = value_of(*upright, b.upright_envelope);
  REQUIRE(env);
  const auto box = std::get<vs::core::dbox3>(*env);
  CHECK(box.min.z == Approx(10).margin(1e-6));
  CHECK(box.max.z == Approx(50).margin(1e-6));

  // The flange: 60 x 10 on z = 10, two M4 holes 40 mm apart.
  const ConnectionBuild* fl = r.find_connection(b.flange);
  REQUIRE(fl);
  CHECK(fl->ok);
  CHECK(fl->area == Approx(600));
  CHECK(fl->frame.origin.z == Approx(10));
  CHECK(fl->frame.normal().z == Approx(1));
  REQUIRE(fl->hole_centres.size() == 2);
  CHECK(fl->hole_centres[0].x == Approx(10));
  CHECK(fl->hole_centres[1].x == Approx(50));
  CHECK(fl->hole_centres[0].y == Approx(5));
  CHECK(fl->hole_centres[0].z == Approx(10));
}

TEST_CASE("Without the project's files, static contents stay stale", "[model][build]") {
  const auto b = bracket::make_bracket();
  const auto r = build(b.project.document, kSettings);
  REQUIRE(r.ok());
  CHECK(r.find_volume(b.upright)->status == VolumeStatus::Stale);
  CHECK(r.find_volume(b.upright)->contents.is_empty());
  CHECK_FALSE(r.find_volume(b.upright)->outputs[0].value.has_value());
}

TEST_CASE("Layout changes reach boundaries, surfaces and contents", "[model][build]") {
  auto b = bracket::make_bracket();
  auto& doc = b.project.document;
  REQUIRE(doc.layout.variables().set_expression(b.base_h, "12 mm").empty());
  const auto r = build(doc, kSettings);
  INFO(codes(r.diagnostics));
  REQUIRE(r.ok());
  CHECK(volume_of(r.find_volume(b.base)->boundary) == Approx(60.0 * 40 * 12));
  CHECK(r.find_connection(b.flange)->frame.origin.z == Approx(12));
  // The contents sit on the flange and are `thickness` = base_h deep.
  CHECK(volume_of(r.find_volume(b.base)->contents) == Approx(60.0 * 40 * 12));
}

TEST_CASE("Keep-outs cut boundaries before contents generate", "[model][build]") {
  auto b = bracket::make_bracket();
  auto& doc = b.project.document;

  SECTION("a keep-out volume as the cutter") {
    const auto diags = edit(doc, [&](Document& d) {
      const Id channel = add_box_volume(d, "Channel", {20, 0, 0}, {40, 10, 10}, true);
      d.keep_outs.push_back({Id::random(), "Cable channel", {channel}, {b.base}});
    });
    INFO(codes(diags));
    REQUIRE_FALSE(diags.has_errors());
    const auto r = build(doc, kSettings);
    INFO(codes(r.diagnostics));
    REQUIRE(r.ok());
    CHECK(volume_of(r.find_volume(b.base)->boundary) == Approx(24000 - 20 * 10 * 10));
    // The keep-out volume itself has no contents, and that is current.
    const auto& channel = r.volumes.back();
    CHECK(channel.status == VolumeStatus::Current);
    CHECK(volume_of(channel.boundary) == Approx(2000));
  }
  SECTION("a layout solid as the cutter") {
    const auto diags = edit(doc, [&](Document& d) {
      d.keep_outs.push_back({Id::random(), "Under the upright", {b.upright_block}, {b.base}});
    });
    REQUIRE_FALSE(diags.has_errors());
    const auto r = build(doc, kSettings);
    REQUIRE(r.ok());
    // The upright sits on the base and doesn't overlap it: nothing to cut.
    CHECK(volume_of(r.find_volume(b.base)->boundary) == Approx(24000));
  }
  SECTION("a keep-out that leaves nothing") {
    const auto diags = edit(doc, [&](Document& d) {
      const Id all = add_box_volume(d, "Everything", {-1, -1, -1}, {61, 41, 11}, true);
      d.keep_outs.push_back({Id::random(), "All of it", {all}, {b.base}});
    });
    REQUIRE_FALSE(diags.has_errors());
    const auto r = build(doc, kSettings);
    INFO(codes(r.diagnostics));
    CHECK(codes(r.diagnostics).find("model.boundary.cut_away") != std::string::npos);
    CHECK(r.find_volume(b.base)->status == VolumeStatus::Failed);
  }
  SECTION("only keep-out volumes and solid features can cut") {
    const auto diags = edit(doc, [&](Document& d) {
      d.keep_outs.push_back({Id::random(), "Bad", {b.upright}, {b.base}});
    });
    CHECK(codes(diags) == "model.keep_out.bad_cutter ");
  }
}

TEST_CASE("Contents anchor to their boundary's faces", "[model][build]") {
  auto b = bracket::make_bracket();
  auto& doc = b.project.document;

  SECTION("a face of a feature boundary, by '<feature id>:<name>'") {
    const auto diags = edit(doc, [&](Document& d) {
      auto* tree = d.find_volume(b.base)->tree();
      vs::sketch::Sketch s(Id::random(), "On the bottom");
      tree->add(vs::feature::SketchFeature{
                    s, vs::feature::FaceRef{b.base, b.base_block.to_string() + ":start"}},
                "Bottom sketch");
    });
    REQUIRE_FALSE(diags.has_errors());
    const auto r = build(doc, kSettings);
    INFO(codes(r.diagnostics));
    const auto& inner = *r.find_volume(b.base)->contents_rebuild;
    const auto* sk = inner.features.back().sketch.get();
    REQUIRE(sk);
    CHECK(sk->plane().origin.z == Approx(0).margin(1e-9));
    CHECK(sk->plane().normal().z == Approx(-1));
  }
  SECTION("a face of a box boundary, by its side") {
    const auto diags = edit(doc, [&](Document& d) {
      const Id box = add_box_volume(d, "Box", {100, 0, 0}, {120, 20, 30}, false);
      ParametricContents pc;
      vs::sketch::Sketch s(Id::random(), "On top");
      pc.tree.add(vs::feature::SketchFeature{s, vs::feature::FaceRef{box, "+z"}}, "Top sketch");
      d.find_volume(box)->contents = std::move(pc);
    });
    REQUIRE_FALSE(diags.has_errors());
    const auto r = build(doc, kSettings);
    INFO(codes(r.diagnostics));
    const auto& vb = r.volumes.back();
    REQUIRE(vb.contents_rebuild);
    const auto* sk = vb.contents_rebuild->features.back().sketch.get();
    REQUIRE(sk);
    CHECK(sk->plane().origin.z == Approx(30));
  }
}

TEST_CASE("Build failures name the part to fix", "[model][build]") {
  auto b = bracket::make_bracket();
  auto& doc = b.project.document;

  SECTION("a suppressed boundary feature fails the boundary, not the model") {
    doc.layout.set_suppressed(b.base_block, true);
    const auto r = build(doc, kSettings, &b.project.blobs);
    const auto failed = r.diagnostics.with_code("model.boundary.feature_failed");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0]->target()->kind == vs::core::SubjectKind::Boundary);
    CHECK(failed[0]->target()->id == b.base);
    CHECK(r.find_volume(b.base)->status == VolumeStatus::Failed);
    CHECK(r.diagnostics.with_code("model.contents.no_boundary").size() == 1);
    // The upright carries on.
    CHECK(r.find_volume(b.upright)->status == VolumeStatus::Current);
  }
  SECTION("contents whose tree fails") {
    auto* f = doc.find_volume(b.base)->tree()->find(b.inner_extrude);
    std::get<vs::feature::ExtrudeFeature>(f->definition).depth = Param::of(-5);
    const auto r = build(doc, kSettings);
    CHECK(r.diagnostics.with_code("model.contents.failed").size() == 1);
    CHECK(r.find_volume(b.base)->status == VolumeStatus::Failed);
  }
  SECTION("a missing blob") {
    auto blobs = b.project.blobs;
    blobs.clear();
    const auto r = build(doc, kSettings, &blobs);
    CHECK(r.diagnostics.with_code("model.contents.missing_blob").size() == 1);
    CHECK(r.find_volume(b.upright)->status == VolumeStatus::Failed);
  }
}

TEST_CASE("Datum patches and bolt circles", "[model][build]") {
  auto b = bracket::make_bracket();
  auto& doc = b.project.document;
  const auto diags = edit(doc, [&](Document& d) {
    auto& fl = *d.find_connection(b.flange);
    DatumPatch dp;
    dp.plane = FeatureRef{b.top_plane};
    dp.u = Param::of(30);
    dp.v = Param::of(5);
    dp.width = Param::expr("width");
    dp.height = Param::of(10);
    fl.geometry = dp;
    BoltCircle circle;
    circle.count = Param::of(4);
    circle.diameter = Param::of(8);
    std::get<BoltedFlangeParams>(fl.params).holes.layout = circle;
  });
  REQUIRE_FALSE(diags.has_errors());
  const auto r = build(doc, kSettings);
  INFO(codes(r.diagnostics));
  REQUIRE(r.ok());
  const auto* fl = r.find_connection(b.flange);
  CHECK(fl->area == Approx(600));
  REQUIRE(fl->hole_centres.size() == 4);
  CHECK(fl->hole_centres[0].x == Approx(34));
  CHECK(fl->hole_centres[0].y == Approx(5));
  CHECK(fl->hole_centres[1].x == Approx(30));
  CHECK(fl->hole_centres[1].y == Approx(9));
  CHECK(vs::kernel::surface_area(fl->patch) == Approx(600));
}

#include <catch2/catch_test_macros.hpp>

#include "bracket.hpp"
#include "vs/model/check.hpp"
#include "vs/model/document.hpp"

using namespace vs::model;
using bracket::codes;

TEST_CASE("The sample bracket passes the authoring checks", "[model][document]") {
  const auto b = bracket::make_bracket();
  const auto& doc = b.project.document;
  const auto findings = check(doc);
  INFO(codes(findings));
  CHECK(findings.empty());

  CHECK(doc.find_volume(b.base)->kind() == VolumeKind::Parametric);
  CHECK(doc.find_volume(b.upright)->kind() == VolumeKind::Static);
  CHECK(doc.find_connection(b.flange)->type() == ConnectionType::BoltedFlange);
  CHECK(doc.find_connection(b.flange)->joins(b.base));
  CHECK(doc.contents_owner(b.inner_extrude) == doc.find_volume(b.base));
  CHECK(doc.contents_owner(b.base_block) == nullptr);
}

TEST_CASE("Volume kinds follow their contents", "[model][document]") {
  Volume v;
  CHECK(v.kind() == VolumeKind::Static);
  CHECK(v.contents_empty());  // a placeholder
  v.contents = ParametricContents{};
  CHECK(v.kind() == VolumeKind::Parametric);
  CHECK(v.contents_empty());
  v.contents = ProceduralContents{"{}"};
  CHECK(v.kind() == VolumeKind::Procedural);
  CHECK_FALSE(v.contents_empty());
}

TEST_CASE("Thread sizes are ISO metric coarse", "[model][document]") {
  REQUIRE(thread_size("M4"));
  CHECK(thread_size("M4")->nominal == 4.0);
  CHECK(thread_size("M4")->clearance_hole == 4.5);
  CHECK_FALSE(thread_size("M7"));
  CHECK(thread_sizes().size() == 9);
}

TEST_CASE("edit() keeps good changes and refuses bad ones", "[model][document]") {
  auto b = bracket::make_bracket();
  Document& doc = b.project.document;

  SECTION("a rename is kept") {
    const auto d = edit(doc, [&](Document& x) { x.find_volume(b.base)->name = "Plinth"; });
    CHECK_FALSE(d.has_errors());
    CHECK(doc.find_volume(b.base)->name == "Plinth");
  }
  SECTION("a duplicate volume name is refused and nothing changes") {
    const Document before = doc;
    const auto d = edit(doc, [&](Document& x) { x.find_volume(b.base)->name = "Upright"; });
    CHECK(codes(d) == "model.volume.duplicate_name ");
    CHECK(doc == before);
  }
  SECTION("an unknown thread is refused") {
    const auto d = edit(doc, [&](Document& x) {
      std::get<BoltedFlangeParams>(x.find_connection(b.flange)->params).thread = "M7";
    });
    CHECK(codes(d) == "model.connection.unknown_thread ");
    REQUIRE(d[0].target());
    CHECK(d[0].target()->id == b.flange);
  }
  SECTION("a connection surface joins one or two distinct volumes") {
    auto d = edit(doc, [&](Document& x) { x.find_connection(b.flange)->volumes = {b.base, b.base}; });
    CHECK(codes(d) == "model.connection.volumes ");
    d = edit(doc, [&](Document& x) {
      ConnectionSurface outside;
      outside.id = Id::random();
      outside.name = "Upright top";
      outside.geometry = DatumPatch{};
      outside.volumes = {b.upright};  // one volume and the outside
      x.connections.push_back(outside);
    });
    CHECK_FALSE(d.has_errors());
  }
  SECTION("a keep-out volume can't have contents or connections") {
    auto d = edit(doc, [&](Document& x) { x.find_volume(b.base)->keep_out = true; });
    CHECK(codes(d) == "model.volume.keep_out_contents model.connection.keep_out ");
  }
  SECTION("ids are unique across the whole document") {
    auto d = edit(doc, [&](Document& x) { x.connections[0].id = b.inner_extrude; });
    CHECK(codes(d).find("model.document.duplicate_id") != std::string::npos);
  }
  SECTION("a boundary must be made of solid features that add") {
    auto d = edit(doc, [&](Document& x) {
      x.find_volume(b.base)->boundary = FeatureBoundary{{b.top_plane}};
    });
    CHECK(codes(d) == "model.boundary.not_solid_feature ");
    d = edit(doc, [&](Document& x) { x.find_volume(b.base)->boundary = FeatureBoundary{}; });
    CHECK(codes(d) == "model.boundary.empty ");
  }
  SECTION("links carry scalar outputs to variables or parameters, one link per target") {
    auto d = edit(doc, [&](Document& x) {
      x.links[0].source = {b.upright, b.upright_envelope};
    });
    CHECK(d.with_code("model.link.not_scalar").size() == 1);
    d = edit(doc, [&](Document& x) { x.links[0].target = b.flange; });
    CHECK(codes(d) == "model.link.bad_target ");
    d = edit(doc, [&](Document& x) {
      x.links.push_back({Id::random(), {b.base, b.base_volume}, b.upright_load});
    });
    CHECK(codes(d) == "model.link.duplicate_target ");
  }
  SECTION("parameter names must be legal and distinct") {
    auto d = edit(doc, [&](Document& x) {
      x.find_volume(b.base)->parameters.push_back({Id::random(), "thickness", Param::of(1)});
    });
    CHECK(codes(d) == "model.volume.bad_parameter ");
    d = edit(doc, [&](Document& x) {
      x.find_volume(b.base)->parameters.push_back({Id::random(), "2x", Param::of(1)});
    });
    CHECK(codes(d) == "model.volume.bad_parameter ");
  }
}

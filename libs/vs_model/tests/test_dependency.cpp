// M5 acceptance: a published output feeding something upstream of its volume
// is rejected with the loop path (spec, design rule 6).

#include <catch2/catch_test_macros.hpp>

#include "bracket.hpp"
#include "vs/model/check.hpp"
#include "vs/model/dependency.hpp"

using namespace vs::model;
using bracket::codes;

TEST_CASE("The bracket's dependency graph", "[model][dependency]") {
  const auto b = bracket::make_bracket();
  const auto g = DependencyGraph::build(b.project.document);
  CHECK(g.loops().empty());
  CHECK_FALSE(g.check().has_errors());

  const auto base_contents = g.find(NodeKind::VolumeContents, b.base);
  const auto upright_contents = g.find(NodeKind::VolumeContents, b.upright);
  const auto base_boundary = g.find(NodeKind::VolumeBoundary, b.base);
  const auto width = g.find(NodeKind::Variable, b.width);
  const auto flange = g.find(NodeKind::ConnectionSurface, b.flange);
  const auto mass = g.find(NodeKind::PublishedOutput, b.base_mass);
  const auto load = g.find(NodeKind::Parameter, b.upright_load);
  REQUIRE(base_contents);
  REQUIRE(upright_contents);
  REQUIRE(base_boundary);
  REQUIRE(width);
  REQUIRE(flange);
  REQUIRE(mass);
  REQUIRE(load);

  CHECK(g.depends_on(*base_contents, *base_boundary));
  CHECK(g.depends_on(*base_contents, *width));    // through the boundary's sketch
  CHECK(g.depends_on(*base_contents, *flange));
  CHECK(g.depends_on(*flange, *width));
  CHECK(g.depends_on(*mass, *base_contents));
  // The link: Base's mass feeds the Upright's load, so the Upright's
  // contents come after the Base's, never the other way.
  CHECK(g.depends_on(*upright_contents, *mass));
  CHECK(g.depends_on(*load, *base_contents));
  CHECK_FALSE(g.depends_on(*base_contents, *upright_contents));

  const auto order = g.order();
  CHECK(order.size() == g.nodes().size());
  const auto pos = [&](std::size_t n) {
    return std::find(order.begin(), order.end(), n) - order.begin();
  };
  CHECK(pos(*base_contents) < pos(*mass));
  CHECK(pos(*mass) < pos(*upright_contents));
  CHECK(pos(*width) < pos(*base_boundary));
}

TEST_CASE("An output feeding something upstream of its volume is a loop",
          "[model][dependency]") {
  auto b = bracket::make_bracket();
  Document& doc = b.project.document;
  const Document before = doc;

  SECTION("Base's mass driving the variable its boundary is sketched with") {
    const Id link = Id::random();
    const auto d = edit(doc, [&](Document& x) {
      x.links.push_back({link, {b.base, b.base_volume}, b.depth});
    });
    INFO(codes(d));
    const auto loops = d.with_code("model.dependency.loop");
    REQUIRE(loops.size() == 1);
    CHECK(doc == before);
    // The path starts and ends at the link, and passes the output, the
    // contents, the boundary, the boundary's feature and sketch, and the
    // variable.
    const auto& msg = loops[0]->message;
    INFO(msg);
    CHECK(msg.find("link 'Base.volume \xE2\x86\x92 depth' \xE2\x86\x92 output 'volume' of 'Base' "
                   "\xE2\x86\x92 contents of 'Base' \xE2\x86\x92 boundary of 'Base' \xE2\x86\x92 "
                   "'Base block' \xE2\x86\x92 'Base outline' \xE2\x86\x92 variable 'depth' "
                   "\xE2\x86\x92 link 'Base.volume \xE2\x86\x92 depth'") != std::string::npos);
    // Subjects: the link first (act here), then the loop in order.
    const auto& s = loops[0]->subjects;
    REQUIRE(s.size() == 7);
    CHECK(s[0].id == link);
    CHECK(s[0].kind == vs::core::SubjectKind::Link);
    CHECK(s[1].id == b.base_volume);
    CHECK(s[2].id == b.base);
    CHECK(s[2].kind == vs::core::SubjectKind::Volume);
    CHECK(s[3].kind == vs::core::SubjectKind::Boundary);
    CHECK(s[4].id == b.base_block);
    CHECK(s[5].id == b.base_sketch);
    CHECK(s[6].id == b.depth);
  }

  SECTION("through a variable that another variable uses") {
    const auto d = edit(doc, [&](Document& x) {
      REQUIRE(x.layout.variables().add("half_w", "20 mm").ok());
      REQUIRE(x.layout.variables().set_expression(b.depth, "half_w * 2").empty());
      // half_w is now fed by the Base's mass: half_w → depth → Base outline.
      const Id half = x.layout.variables().find("half_w")->id;
      x.links.push_back({Id::random(), {b.base, b.base_mass}, half});
    });
    INFO(codes(d));
    CHECK(d.with_code("model.dependency.loop").size() == 1);
    CHECK(doc == before);
  }

  SECTION("through the flange the contents sit on") {
    const auto d = edit(doc, [&](Document& x) {
      x.links.push_back({Id::random(), {b.base, b.base_mass}, b.upright_t});
    });
    INFO(codes(d));
    const auto loops = d.with_code("model.dependency.loop");
    REQUIRE(loops.size() >= 1);
    CHECK(loops[0]->message.find("connection surface 'Flange'") != std::string::npos);
  }

  SECTION("through a parameter of the same volume") {
    const auto d = edit(doc, [&](Document& x) {
      x.links.push_back({Id::random(), {b.base, b.base_mass}, b.base_thickness});
    });
    INFO(codes(d));
    const auto loops = d.with_code("model.dependency.loop");
    REQUIRE(loops.size() == 1);
    CHECK(loops[0]->message.find("parameter 'thickness' of 'Base'") != std::string::npos);
  }

  SECTION("through a keep-out that cuts the volume") {
    const auto d = edit(doc, [&](Document& x) {
      // A keep-out volume sized by a variable fed from the Base's mass.
      const Id channel_w = x.layout.variables().add("channel_w", "20 mm").value();
      const Id lo = x.layout.add(vs::feature::DatumPointFeature{vs::feature::PointAt{
                                     Param::of(20), Param::of(10), Param::of(5)}},
                                 "Channel lo");
      const Id hi = x.layout.add(vs::feature::DatumPointFeature{vs::feature::PointAt{
                                     Param::expr("20 + channel_w"), Param::of(30), Param::of(15)}},
                                 "Channel hi");
      Volume channel;
      channel.id = Id::random();
      channel.name = "Channel";
      channel.keep_out = true;
      channel.boundary = BoxBoundary{vs::feature::FeatureRef{lo}, vs::feature::FeatureRef{hi}};
      x.volumes.push_back(channel);
      x.keep_outs.push_back({Id::random(), "Cable channel", {channel.id}, {b.base}});
      x.links.push_back({Id::random(), {b.base, b.base_mass}, channel_w});
    });
    INFO(codes(d));
    const auto loops = d.with_code("model.dependency.loop");
    REQUIRE(loops.size() == 1);
    CHECK(loops[0]->message.find("keep-out 'Cable channel'") != std::string::npos);
    CHECK(loops[0]->message.find("boundary of 'Channel'") != std::string::npos);
  }

  SECTION("feeding a sibling that isn't upstream is fine") {
    // The bracket already links Base.mass to Upright.load; a second output
    // feeding a layout variable nothing upstream of Base uses is fine too.
    const auto d = edit(doc, [&](Document& x) {
      const Id report = x.layout.variables().add("report_volume", "0").value();
      x.links.push_back({Id::random(), {b.base, b.base_volume}, report});
    });
    INFO(codes(d));
    CHECK_FALSE(d.has_errors());
  }
}

TEST_CASE("Loops among variables alone stay the variable table's", "[model][dependency]") {
  auto b = bracket::make_bracket();
  Document copy = b.project.document;
  copy.layout.variables().add_unchecked({Id::random(), "p", "q"});
  copy.layout.variables().add_unchecked({Id::random(), "q", "p"});
  const auto d = check(copy);
  CHECK(d.with_code("feature.variable.loop").size() >= 1);
  CHECK(d.with_code("model.dependency.loop").empty());
}

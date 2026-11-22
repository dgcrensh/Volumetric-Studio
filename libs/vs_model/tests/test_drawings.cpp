// L6: drawings in the document (plan docs/plans/L6-drawings.md, "Checks").

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "bracket.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"
#include "vs/model/configuration.hpp"
#include "vs/model/drawing.hpp"
#include "vs/model/project.hpp"

using namespace vs::model;
using bracket::codes;
using vs::core::Id;

namespace {

bool has(const vs::core::Diagnostics& ds, std::string_view code) {
  return std::any_of(ds.begin(), ds.end(), [&](const auto& d) { return d.code == code; });
}

Anchor on_boundary(const Id& volume, AnchorPart part, std::string face = {}) {
  return {AnchorOn::VolumeBoundary, volume, part, std::move(face), 0};
}

// The bracket with one drawing: a front view of both volumes and three
// dimensions (stable width, disposable contents height, a bolt hole).
struct Drawn {
  bracket::Bracket b;
  Id drawing, front, top, width_dim, contents_dim, hole_dim;
};

Drawn drawn() {
  Drawn out{bracket::make_bracket(), {}, {}, {}, {}, {}, {}};
  Document& doc = out.b.project.document;
  Drawing d = make_drawing("Bracket");
  DrawingView front;
  front.id = Id::random();
  front.name = "Front";
  front.direction = ViewDirection::Front;
  front.x = 150;
  front.y = 100;
  DrawingView top = front;
  top.id = Id::random();
  top.name = "Top";
  top.direction = ViewDirection::Top;
  top.volumes = {out.b.base};
  top.hidden = true;
  top.y = 220;
  top.scale = 2;
  d.views = {front, top};
  DrawingDimension width{Id::random(), front.id, DimensionKind::Horizontal,
                         on_boundary(out.b.base, AnchorPart::Min),
                         on_boundary(out.b.base, AnchorPart::Max), 12.5};
  DrawingDimension contents{
      Id::random(), front.id, DimensionKind::Vertical,
      {AnchorOn::VolumeContents, out.b.base, AnchorPart::Face,
       out.b.inner_extrude.to_string() + ":end", 0},
      {AnchorOn::Origin, {}, AnchorPart::Centre, {}, 0}, -8};
  DrawingDimension hole{Id::random(), top.id, DimensionKind::Diameter,
                        {AnchorOn::Surface, out.b.flange, AnchorPart::Hole, {}, 1}, {}, 6};
  d.dimensions = {width, contents, hole};
  out.drawing = d.id;
  out.front = front.id;
  out.top = top.id;
  out.width_dim = width.id;
  out.contents_dim = contents.id;
  out.hole_dim = hole.id;
  doc.drawings.push_back(std::move(d));
  return out;
}

}  // namespace

TEST_CASE("Drawings round-trip and leave other documents unchanged", "[model][drawing]") {
  auto b = bracket::make_bracket();
  const auto before = to_json(b.project.document);
  CHECK(before.find("drawings") == std::string::npos);

  auto d = drawn();
  const Document& doc = d.b.project.document;
  CHECK(check(doc).empty());
  const auto text = to_json(doc);
  const auto back = document_from_json(text);
  REQUIRE(back);
  CHECK(*back == doc);
  CHECK(to_json(*back) == text);
  CHECK(content_hash(*back) == content_hash(doc));

  const Drawing* dr = doc.find_drawing("Bracket");
  REQUIRE(dr);
  CHECK(dr == doc.find_drawing(d.drawing));
  CHECK(dr->find_view(d.top)->scale == 2);
  CHECK_FALSE(dr->find_dimension(d.width_dim)->disposable());
  CHECK(dr->find_dimension(d.contents_dim)->disposable());
  CHECK_FALSE(dr->find_dimension(d.hole_dim)->disposable());
  CHECK(doc.drawing_subject(d.top).label == "view 'Top' of drawing 'Bracket'");
  CHECK(doc.drawing_subject(d.hole_dim).label == "dimension 3 of drawing 'Bracket'");

  // Drawings don't build: the layout's inputs leave them out.
  Document bare = doc;
  bare.drawings.clear();
  CHECK(layout_inputs(doc) == layout_inputs(bare));
}

TEST_CASE("Drawings read with defaults and refuse bad values", "[model][drawing]") {
  auto d = drawn();
  auto text = to_json(d.b.project.document);
  // An unknown direction is a file error, not a check.
  auto bad = text;
  const auto at = bad.find("\"top\"");
  REQUIRE(at != std::string::npos);
  bad.replace(at, 5, "\"up\"");
  const auto r = document_from_json(bad);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == "model.json.bad_value");
}

TEST_CASE("check() refuses drawings that name nothing", "[model][drawing]") {
  auto expect = [](auto edit, std::string_view code) {
    auto d = drawn();
    Document& doc = d.b.project.document;
    edit(d, doc.drawings.front());
    const auto ds = check(doc);
    INFO(codes(ds));
    CHECK(has(ds, code));
  };
  expect([](Drawn&, Drawing& dr) { dr.name.clear(); }, "model.drawing.name");
  expect([](Drawn& d, Drawing& dr) {
    Drawing copy = make_drawing(dr.name);
    d.b.project.document.drawings.push_back(copy);
  }, "model.drawing.name");
  expect([](Drawn&, Drawing& dr) { dr.width = 0; }, "model.drawing.sheet");
  expect([](Drawn&, Drawing& dr) { dr.decimals = 9; }, "model.drawing.sheet");
  expect([](Drawn&, Drawing& dr) { dr.views[0].scale = -1; }, "model.drawing.sheet");
  expect([](Drawn&, Drawing& dr) { dr.views[0].volumes = {Id::random()}; },
         "model.drawing.volume");
  expect([](Drawn&, Drawing& dr) { dr.dimensions[0].view = Id::random(); },
         "model.drawing.view");
  expect([](Drawn&, Drawing& dr) { dr.dimensions[0].from.target = Id::random(); },
         "model.drawing.anchor");
  // A surface where a volume belongs.
  expect([](Drawn& d, Drawing& dr) { dr.dimensions[0].from.target = d.b.flange; },
         "model.drawing.anchor");
  expect([](Drawn&, Drawing& dr) { dr.dimensions[0].from.part = AnchorPart::Hole; },
         "model.drawing.anchor");
  expect([](Drawn&, Drawing& dr) { dr.dimensions[0].from.face.clear(),
                                   dr.dimensions[0].from.part = AnchorPart::Face; },
         "model.drawing.anchor");
  // A diameter of a box's extreme, of the origin, of a planar patch.
  expect([](Drawn&, Drawing& dr) { dr.dimensions[0].kind = DimensionKind::Diameter; },
         "model.drawing.anchor");
  expect([](Drawn&, Drawing& dr) {
    dr.dimensions[2].from = {AnchorOn::Origin, {}, AnchorPart::Centre, {}, 0};
  }, "model.drawing.anchor");
  expect([](Drawn&, Drawing& dr) { dr.dimensions[2].from.part = AnchorPart::Centre; },
         "model.drawing.anchor");
  // Ids are unique across the document, drawings included.
  expect([](Drawn& d, Drawing& dr) { dr.views[1].id = d.b.base; }, "model.document.duplicate_id");
}

TEST_CASE("Nothing can drive a drawing dimension", "[model][drawing]") {
  auto d = drawn();
  Document& doc = d.b.project.document;
  // A link to a dimension is refused as a link to anything that isn't a
  // variable or parameter is.
  Link l = *std::find_if(doc.links.begin(), doc.links.end(),
                         [&](const Link& x) { return x.id == d.b.link; });
  l.id = Id::random();
  l.target = d.width_dim;
  Document linked = doc;
  linked.links.push_back(l);
  CHECK(has(check(linked), "model.link.bad_target"));
  // So is a configuration value.
  Document configured_doc = doc;
  configured_doc.configurations.push_back({Id::random(), "Big", {{d.width_dim, "80"}}});
  CHECK(has(check(configured_doc), "model.configuration.target"));
}

TEST_CASE("Pruning drops what an edit removed", "[model][drawing]") {
  auto d = drawn();
  Document& doc = d.b.project.document;
  CHECK(prune_drawings(doc) == 0);
  // Remove the Base volume (and what names it): the top view shows nothing
  // it can name, and both dimensions on Base go; the hole on the flange
  // stays until the flange goes too.
  Document cut = doc;
  std::erase_if(cut.volumes, [&](const Volume& v) { return v.id == d.b.base; });
  CHECK(prune_drawings(cut) == 3);
  const Drawing& dr = cut.drawings.front();
  CHECK(dr.find_view(d.top)->volumes.empty());
  REQUIRE(dr.dimensions.size() == 1);
  CHECK(dr.dimensions[0].id == d.hole_dim);
  std::erase_if(cut.connections, [&](const ConnectionSurface& c) { return c.id == d.b.flange; });
  CHECK(prune_drawings(cut) == 1);
  CHECK(cut.drawings.front().dimensions.empty());
  // A view removed takes its dimensions with it.
  Document no_view = doc;
  no_view.drawings.front().views.erase(no_view.drawings.front().views.begin());
  CHECK(prune_drawings(no_view) == 2);
}

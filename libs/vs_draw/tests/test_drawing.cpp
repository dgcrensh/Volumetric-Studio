// L6: drawings from evaluations (plan docs/plans/L6-drawings.md, "Tests").

#include <algorithm>
#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "bracket.hpp"
#include "rack.hpp"
#include "vs/draw/drawing.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/check.hpp"
#include "vs/model/drawing.hpp"
#include "vs/model/project.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/graph.hpp"

using namespace vs;
using namespace vs::draw;
using Catch::Matchers::WithinAbs;
using core::Id;
using model::Anchor;
using model::AnchorOn;
using model::AnchorPart;
using model::DimensionKind;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

bool has(const core::Diagnostics& ds, std::string_view code) {
  return std::any_of(ds.begin(), ds.end(), [&](const auto& d) { return d.code == code; });
}

std::string codes(const core::Diagnostics& ds) {
  std::string s;
  for (const auto& d : ds) s += d.code + ": " + d.message + "\n";
  return s;
}

Anchor boundary(const Id& v, AnchorPart part, std::string face = {}) {
  return {AnchorOn::VolumeBoundary, v, part, std::move(face), 0};
}
Anchor contents(const Id& v, AnchorPart part, std::string face = {}) {
  return {AnchorOn::VolumeContents, v, part, std::move(face), 0};
}
Anchor surface(const Id& s, AnchorPart part, std::uint32_t hole = 0) {
  return {AnchorOn::Surface, s, part, {}, hole};
}
const Anchor kOrigin{AnchorOn::Origin, {}, AnchorPart::Centre, {}, 0};

model::DrawingView make_view(std::string name, model::ViewDirection dir, double x, double y,
                             std::vector<Id> volumes = {}) {
  model::DrawingView v;
  v.id = Id::random();
  v.name = std::move(name);
  v.direction = dir;
  v.x = x;
  v.y = y;
  v.volumes = std::move(volumes);
  return v;
}

Id add_dim(model::Drawing& d, const Id& view, DimensionKind kind, Anchor from, Anchor to = {},
           double offset = 10) {
  d.dimensions.push_back({Id::random(), view, kind, std::move(from), std::move(to), offset});
  return d.dimensions.back().id;
}

double value(const DrawingResult& r, const Id& dim) {
  const auto* d = r.find(dim);
  REQUIRE(d);
  REQUIRE(d->value);
  return *d->value;
}

// The bracket with a drawing of stable and disposable dimensions.
struct BracketDrawing {
  bracket::Bracket b;
  Id front, top;
  Id width, height, hole, thickness, depth_to_hole, contents_width;
};

BracketDrawing bracket_drawing() {
  BracketDrawing out{bracket::make_bracket(), {}, {}, {}, {}, {}, {}, {}, {}};
  auto& b = out.b;
  model::Drawing d = model::make_drawing("Bracket");
  const auto front = make_view("Front", model::ViewDirection::Front, 120, 100);
  auto top = make_view("Top", model::ViewDirection::Top, 120, 220);
  top.scale = 2;
  d.views = {front, top};
  out.front = front.id;
  out.top = top.id;
  out.width = add_dim(d, front.id, DimensionKind::Horizontal, boundary(b.base, AnchorPart::Min),
                      boundary(b.base, AnchorPart::Max), -10);
  out.height = add_dim(d, front.id, DimensionKind::Vertical, boundary(b.base, AnchorPart::Min),
                       boundary(b.upright, AnchorPart::Max), -10);
  out.hole = add_dim(d, top.id, DimensionKind::Diameter, surface(b.flange, AnchorPart::Hole, 1));
  const std::string fill = b.inner_extrude.to_string();
  out.thickness = add_dim(d, front.id, DimensionKind::Vertical,
                          contents(b.base, AnchorPart::Face, fill + ":start"),
                          contents(b.base, AnchorPart::Face, fill + ":end"), 10);
  out.depth_to_hole = add_dim(d, top.id, DimensionKind::Horizontal, kOrigin,
                              surface(b.flange, AnchorPart::Hole, 1), 15);
  out.contents_width = add_dim(d, front.id, DimensionKind::Horizontal,
                               contents(b.base, AnchorPart::Min), contents(b.base, AnchorPart::Max));
  b.project.document.drawings.push_back(std::move(d));
  REQUIRE(model::check(b.project.document).empty());
  return out;
}

DrawOptions options() {
  DrawOptions o;
  o.settings = kSettings;
  return o;
}

}  // namespace

TEST_CASE("The bracket's drawing measures stable and disposable dimensions", "[draw][drawing]") {
  auto bd = bracket_drawing();
  eval::Engine engine(kSettings);
  const auto ev = engine.evaluate(bd.b.project);
  REQUIRE(ev.ok());
  const auto r = draw::draw(ev, std::string_view("Bracket"), options());
  INFO(codes(r.diagnostics));
  CHECK(r.diagnostics.empty());
  CHECK(r.ok());

  CHECK_THAT(value(r, bd.width), WithinAbs(60, 1e-9));
  CHECK_THAT(value(r, bd.height), WithinAbs(50, 1e-9));
  CHECK_THAT(value(r, bd.hole), WithinAbs(4.5, 1e-12));  // M4, ISO 273 medium
  CHECK_THAT(value(r, bd.depth_to_hole), WithinAbs(50, 1e-9));
  CHECK_THAT(value(r, bd.thickness), WithinAbs(10, 1e-9));
  CHECK_THAT(value(r, bd.contents_width), WithinAbs(60, 1e-9));
  CHECK_FALSE(r.find(bd.width)->disposable);
  CHECK(r.find(bd.thickness)->disposable);
  CHECK(r.find(bd.width)->text == "60");
  CHECK(r.find(bd.thickness)->text == "(10)");
  CHECK(r.find(bd.hole)->text == "⌀4.5");
  for (const auto& d : r.dimensions) CHECK(d.drawn);

  // Both views drew something, the front one in black (contents) and
  // phantom (boundaries), with the flange's outline.
  REQUIRE(r.views.size() == 2);
  for (const auto& v : r.views) {
    CHECK(v.visible_lines > 0);
    CHECK(v.outline_lines > 0);
  }
  const auto pens = [&](Pen pen) {
    return std::count_if(r.sheet.paths.begin(), r.sheet.paths.end(),
                         [&](const SheetPath& p) { return p.pen == pen; });
  };
  CHECK(pens(Pen::Visible) > 0);
  CHECK(pens(Pen::Boundary) > 0);
  CHECK(pens(Pen::Surface) > 0);
  CHECK(pens(Pen::Disposable) > 0);
  CHECK(pens(Pen::Border) >= 2);
  // The front view's lines sit around its centre: the shown boundaries'
  // box (60 x 50) is centred on (120, 100) at 1:1.
  double lo = 1e9, hi = -1e9;
  for (const auto& p : r.sheet.paths) {
    if (p.pen != Pen::Boundary) continue;
    for (const auto& q : p.points) {
      if (q.y > 150) continue;  // the top view is above
      lo = std::min(lo, q.x);
      hi = std::max(hi, q.x);
    }
  }
  CHECK_THAT(lo, WithinAbs(90, 1e-6));
  CHECK_THAT(hi, WithinAbs(150, 1e-6));
  // Text: the title block and labels.
  const auto& texts = r.sheet.texts;
  CHECK(std::any_of(texts.begin(), texts.end(), [](const SheetText& t) {
    return t.text == "Front  1:1";
  }));
  CHECK(std::any_of(texts.begin(), texts.end(), [](const SheetText& t) {
    return t.text == "Top  2:1";
  }));
  CHECK(std::any_of(texts.begin(), texts.end(),
                    [](const SheetText& t) { return t.text == "Document  Bracket"; }));
}

TEST_CASE("Disposable dimensions follow their contents; stable ones don't move",
          "[draw][drawing]") {
  auto bd = bracket_drawing();
  eval::Engine engine(kSettings);
  const auto before = draw::draw(engine.evaluate(bd.b.project), std::string_view("Bracket"), options());
  // Thinner contents: the layout is the same, the contents regenerate.
  auto* base = bd.b.project.document.find_volume(bd.b.base);
  base->parameters[0].value = feature::Param::of(8);
  const auto ev = engine.evaluate(bd.b.project);
  REQUIRE(ev.ok());
  CHECK(ev.stats.regenerated == 1);
  const auto after = draw::draw(ev, std::string_view("Bracket"), options());
  INFO(codes(after.diagnostics));
  CHECK(after.ok());
  CHECK_THAT(value(after, bd.thickness), WithinAbs(8, 1e-9));
  CHECK(after.find(bd.thickness)->text == "(8)");
  for (const Id& stable : {bd.width, bd.height, bd.hole, bd.depth_to_hole}) {
    CHECK(value(after, stable) == value(before, stable));
  }

  // Swapping the contents to a placeholder loses the disposable dimensions
  // (warnings, not drawn); the stable ones stay.
  base->contents = model::StaticContents{};
  const auto empty = draw::draw(engine.evaluate(bd.b.project), std::string_view("Bracket"), options());
  INFO(codes(empty.diagnostics));
  CHECK(empty.ok());
  CHECK(has(empty.diagnostics, "draw.dimension.lost"));
  CHECK_FALSE(empty.find(bd.thickness)->drawn);
  CHECK_FALSE(empty.find(bd.contents_width)->drawn);
  CHECK(empty.find(bd.width)->drawn);
  for (const auto& d : empty.diagnostics) {
    if (d.code == "draw.dimension.lost") {
      CHECK(d.subjects.front().kind == core::SubjectKind::Drawing);
      CHECK_FALSE(d.is_error());
    }
  }
}

TEST_CASE("Stable anchors that don't fit are errors; disposable ones are lost",
          "[draw][drawing]") {
  auto bd = bracket_drawing();
  auto& d = bd.b.project.document.drawings.front();
  // A plane facing up, on a horizontal dimension.
  const Id sideways = add_dim(d, bd.front, DimensionKind::Horizontal, kOrigin,
                              boundary(bd.b.base, AnchorPart::Face,
                                       bd.b.base_block.to_string() + ":end"));
  // No such face; no such hole.
  const Id no_face = add_dim(d, bd.front, DimensionKind::Horizontal, kOrigin,
                             boundary(bd.b.base, AnchorPart::Face, "nope"));
  const Id no_hole =
      add_dim(d, bd.top, DimensionKind::Diameter, surface(bd.b.flange, AnchorPart::Hole, 7));
  // A bolt hole's diameter seen from the front: not a circle.
  const Id edge_on =
      add_dim(d, bd.front, DimensionKind::Diameter, surface(bd.b.flange, AnchorPart::Hole, 0));
  // The contents' face, the wrong way: lost, not an error.
  const Id lost = add_dim(d, bd.front, DimensionKind::Horizontal, kOrigin,
                          contents(bd.b.base, AnchorPart::Face,
                                   bd.b.inner_extrude.to_string() + ":start"));
  REQUIRE(model::check(bd.b.project.document).empty());
  eval::Engine engine(kSettings);
  const auto r = draw::draw(engine.evaluate(bd.b.project), std::string_view("Bracket"), options());
  auto code_of = [&](const Id& dim) {
    for (const auto& x : r.diagnostics) {
      if (x.subjects.front().id == dim) return x.code;
    }
    return std::string{};
  };
  CHECK(code_of(sideways) == "draw.dimension.not_parallel");
  CHECK(code_of(no_face) == "draw.dimension.unresolved");
  CHECK(code_of(no_hole) == "draw.dimension.unresolved");
  CHECK(code_of(edge_on) == "draw.dimension.not_circle");
  CHECK(code_of(lost) == "draw.dimension.lost");
  CHECK_FALSE(r.ok());
  for (const Id& id : {sideways, no_face, no_hole, edge_on, lost}) CHECK_FALSE(r.find(id)->drawn);
  // The good ones still draw.
  CHECK(r.find(bd.width)->drawn);

  CHECK(has(draw::draw(engine.evaluate(bd.b.project), std::string_view("Nope"), options()).diagnostics,
            "draw.drawing.missing"));
}

TEST_CASE("A preview's contents are stale on the drawing", "[draw][drawing]") {
  auto bd = bracket_drawing();
  eval::Engine engine(kSettings);
  const auto ev = engine.preview(bd.b.project);
  const auto r = draw::draw(ev, std::string_view("Bracket"), options());
  INFO(codes(r.diagnostics));
  CHECK(has(r.diagnostics, "draw.contents.stale"));
  CHECK(r.find(bd.width)->drawn);
  CHECK_FALSE(r.find(bd.thickness)->drawn);
  CHECK(r.ok());
}

TEST_CASE("A drawing's files are the same on any thread count", "[draw][drawing]") {
  auto bd = bracket_drawing();
  auto& d = bd.b.project.document.drawings.front();
  d.views.push_back(make_view("Iso", model::ViewDirection::Isometric, 300, 150));
  d.views.back().hidden = true;
  eval::Engine engine(kSettings);
  const auto ev = engine.evaluate(bd.b.project);
  auto o = options();
  const auto a = draw::draw(ev, d.id, o);
  o.parallel = [](std::size_t n, const std::function<void(std::size_t)>& task) {
    for (std::size_t i = n; i-- > 0;) task(i);
  };
  const auto b = draw::draw(ev, d.id, o);
  CHECK(to_svg(a.sheet) == to_svg(b.sheet));
  CHECK(to_pdf(a.sheet) == to_pdf(b.sheet));
  CHECK(to_dxf(a.sheet) == to_dxf(b.sheet));
}

TEST_CASE("The standard drawing fits the sheet and dimensions the layout", "[draw][drawing]") {
  auto b = bracket::make_bracket();
  eval::Engine engine(kSettings);
  const auto ev = engine.evaluate(b.project);
  const auto d = standard_drawing(ev.document, ev.build, "Overview");
  REQUIRE(d.views.size() == 4);
  for (const auto& v : d.views) CHECK(v.scale == 1);  // 60 x 40 x 50 fits A3 at 1:1
  REQUIRE(d.dimensions.size() == 3);
  b.project.document.drawings.push_back(d);
  CHECK(model::check(b.project.document).empty());
  const auto r = draw::draw(ev, d, options());
  INFO(codes(r.diagnostics));
  CHECK(r.ok());
  CHECK_THAT(value(r, d.dimensions[0].id), WithinAbs(60, 1e-9));
  CHECK_THAT(value(r, d.dimensions[1].id), WithinAbs(50, 1e-9));
  CHECK_THAT(value(r, d.dimensions[2].id), WithinAbs(40, 1e-9));
  // Everything drawn is on the sheet, inside the border.
  for (const auto& p : r.sheet.paths) {
    for (const auto& q : p.points) {
      CHECK(q.x >= 10 - 1e-9);
      CHECK(q.x <= 410 + 1e-9);
      CHECK(q.y >= 10 - 1e-9);
      CHECK(q.y <= 287 + 1e-9);
    }
  }
  // A small sheet takes a smaller scale.
  const auto small = standard_drawing(ev.document, ev.build, "Small", 210, 148);
  CHECK(small.views.front().scale < 1);
  CHECK(small.views.front().scale >= 0.2);
}

namespace {

// The rack's generator: a 3 x 2 grid of cells, the seat passed to the one
// at the origin (as vs_proc's tests make it).
std::string grid_generator(const Id& seat) {
  using proc::Setting;
  proc::Graph g;
  const Id grid = g.add("children.grid", "Grid",
                        {{"count", Setting(core::dvec3{3, 2, 1})},
                         {"template", Setting(std::string("Cell"))},
                         {"join", Setting(std::string("bonded"))}});
  const Id delegate =
      g.add("children.delegate", "Seat", {{"surface", Setting(seat.to_string())}});
  const Id out = g.add("output.children", "Children");
  g.link(grid, "children", delegate, "children");
  g.link(delegate, "children", out, "children");
  return proc::to_json(g);
}

}  // namespace

TEST_CASE("Generated children are drawn and measured through their composite",
          "[draw][drawing]") {
  rack::Rack rr = rack::make_rack();
  auto& doc = rr.project.document;
  std::get<model::CompositeContents>(doc.find_volume(rr.rack)->contents).generator =
      grid_generator(rr.seat);
  model::Drawing d = model::make_drawing("Rack");
  const auto top = make_view("Top", model::ViewDirection::Top, 100, 100, {rr.rack});
  d.views = {top};
  const Id across = add_dim(d, top.id, DimensionKind::Horizontal,
                            contents(rr.rack, AnchorPart::Min), contents(rr.rack, AnchorPart::Max));
  const Id box = add_dim(d, top.id, DimensionKind::Horizontal, boundary(rr.rack, AnchorPart::Min),
                         boundary(rr.rack, AnchorPart::Max), -10);
  doc.drawings.push_back(d);
  REQUIRE(model::check(doc).empty());

  eval::Engine engine(kSettings);
  proc::install(engine);
  const auto ev = engine.evaluate(rr.project);
  INFO(codes(ev.diagnostics));
  REQUIRE(ev.ok());
  const auto r = draw::draw(ev, d.id, options());
  INFO(codes(r.diagnostics));
  CHECK(r.ok());
  // Six cells of 20 x 20 fill the 60 x 40 rack: their contents span it.
  CHECK_THAT(value(r, across), WithinAbs(60, 1e-6));
  CHECK(r.find(across)->disposable);
  CHECK_THAT(value(r, box), WithinAbs(60, 1e-9));
  CHECK(r.views.front().visible_lines > 0);
  // Every cell's contents is drawn: lines from six generated volumes.
  std::set<Id> sources;
  for (const auto& p : r.sheet.paths) {
    if (p.pen == Pen::Visible) sources.insert(p.source);
  }
  CHECK(sources.size() == 6);
}

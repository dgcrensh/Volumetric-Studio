// L6 in the app's rules: drawing commands undo to identical hashes,
// deletions prune views and dimensions, and what the Drawings dock and the
// dimension dialog list (plan docs/plans/L6-drawings.md, "App").

#include <algorithm>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "authoring.hpp"
#include "drawings.hpp"
#include "history.hpp"
#include "samples.hpp"
#include "vs/draw/drawing.hpp"
#include "vs/model/check.hpp"
#include "vs/model/drawing.hpp"
#include "vs/model/project.hpp"

using namespace vs;
using app::AnchorChoice;
using app::History;
using core::Id;
using model::AnchorOn;
using model::AnchorPart;
using model::DimensionKind;

namespace {

core::Hash256 hash(const History& h) { return model::content_hash(h.project()); }

void apply(History& h, const app::Command& c) {
  const auto d = h.apply(c);
  INFO(samples::codes(d));
  REQUIRE_FALSE(d.has_errors());
}

Id volume(const model::Document& d, std::string_view name) {
  const model::Volume* v = d.find_volume(name);
  REQUIRE(v != nullptr);
  return v->id;
}

Id surface(const model::Document& d, std::string_view name) {
  for (const auto& c : d.connections) {
    if (c.name == name) return c.id;
  }
  FAIL("no surface " << name);
  return {};
}

const AnchorChoice* find(const std::vector<AnchorChoice>& cs, AnchorOn on, const Id& target, AnchorPart part,
                         std::string_view face = {}) {
  for (const auto& c : cs) {
    if (c.anchor.on == on && c.anchor.target == target && c.anchor.part == part &&
        (face.empty() || c.anchor.face == face)) {
      return &c;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("Drawing commands undo to identical hashes", "[app][drawing]") {
  History h(samples::load("bracket.vsproj"));
  const auto h0 = hash(h);
  const auto ev = samples::evaluate(h.project());
  REQUIRE(ev.ok());

  // A standard drawing, then a second one named the same: made unique.
  model::Drawing standard = draw::standard_drawing(ev.document, ev.build, "Sheet");
  const Id drawing = standard.id;
  REQUIRE_FALSE(standard.views.empty());
  apply(h, app::add_drawing(standard));
  const auto h1 = hash(h);
  CHECK(h1 != h0);
  apply(h, app::add_drawing(model::make_drawing("Sheet")));
  REQUIRE(h.project().document.drawings.size() == 2);
  CHECK(h.project().document.drawings[1].name == "Sheet 2");
  h.undo();
  CHECK(hash(h) == h1);

  apply(h, app::rename_drawing(drawing, "Bracket"));
  CHECK(h.project().document.find_drawing(drawing)->name == "Bracket");
  apply(h, app::set_sheet(drawing, 297, 210, 1));
  CHECK(h.project().document.find_drawing(drawing)->decimals == 1);

  // Views: added with a unique name, changed, removed with their dimensions.
  model::DrawingView back;
  back.direction = model::ViewDirection::Back;
  back.x = 50;
  back.y = 50;
  back.id = Id::random();
  apply(h, app::add_view(drawing, back));
  const model::Drawing* d = h.project().document.find_drawing(drawing);
  REQUIRE(d->find_view(back.id) != nullptr);
  CHECK(d->find_view(back.id)->name == "back");
  back.name = "Behind";
  back.hidden = true;
  apply(h, app::update_view(drawing, back));
  CHECK(h.project().document.find_drawing(drawing)->find_view(back.id)->hidden);

  const Id base = volume(h.project().document, "Base");
  model::DrawingDimension dim;
  dim.id = Id::random();
  dim.view = back.id;
  dim.kind = DimensionKind::Horizontal;
  dim.from = {AnchorOn::VolumeBoundary, base, AnchorPart::Min, {}, 0};
  dim.to = {AnchorOn::VolumeContents, base, AnchorPart::Max, {}, 0};
  apply(h, app::add_dimension(drawing, dim));
  apply(h, app::move_dimension(drawing, dim.id, -20));
  CHECK(h.project().document.find_drawing(drawing)->find_dimension(dim.id)->offset == -20);
  dim.kind = DimensionKind::Vertical;
  apply(h, app::update_dimension(drawing, dim));
  CHECK(h.project().document.find_drawing(drawing)->find_dimension(dim.id)->kind == DimensionKind::Vertical);

  const std::size_t dims = h.project().document.find_drawing(drawing)->dimensions.size();
  apply(h, app::remove_view(drawing, back.id));
  d = h.project().document.find_drawing(drawing);
  CHECK(d->find_view(back.id) == nullptr);
  CHECK(d->dimensions.size() == dims - 1);
  h.undo();
  CHECK(h.project().document.find_drawing(drawing)->find_dimension(dim.id) != nullptr);
  apply(h, app::remove_dimension(drawing, dim.id));
  CHECK(h.project().document.find_drawing(drawing)->find_dimension(dim.id) == nullptr);

  // Missing things are refused.
  CHECK(h.apply(app::remove_drawing(Id::random())).has_errors());
  CHECK(h.apply(app::remove_view(drawing, Id::random())).has_errors());
  CHECK(h.apply(app::move_dimension(drawing, Id::random(), 3)).has_errors());

  apply(h, app::remove_drawing(drawing));
  CHECK(h.project().document.drawings.empty());
  CHECK(hash(h) == h0);  // a document without drawings hashes as before
  while (h.can_undo()) h.undo();
  CHECK(hash(h) == h0);
}

TEST_CASE("Deleting a volume prunes what drawings showed of it", "[app][drawing]") {
  History h(samples::load("bracket.vsproj"));
  const model::Document& doc = h.project().document;
  const Id base = volume(doc, "Base");
  const Id upright = volume(doc, "Upright");
  model::Drawing d = model::make_drawing("Sheet");
  model::DrawingView v;
  v.id = Id::random();
  v.name = "Front";
  v.volumes = {base, upright};
  d.views.push_back(v);
  d.dimensions.push_back({Id::random(), v.id, DimensionKind::Horizontal,
                          {AnchorOn::VolumeBoundary, base, AnchorPart::Min, {}, 0},
                          {AnchorOn::VolumeBoundary, upright, AnchorPart::Max, {}, 0}, 10});
  d.dimensions.push_back({Id::random(), v.id, DimensionKind::Vertical,
                          {AnchorOn::Origin, {}, AnchorPart::Centre, {}, 0},
                          {AnchorOn::VolumeContents, base, AnchorPart::Max, {}, 0}, 10});
  const Id drawing = d.id;
  apply(h, app::add_drawing(d));

  // The flange joins both: deleting the upright takes it, a view's volume
  // and the dimension on it, never refused.
  apply(h, app::remove_volume(upright));
  const model::Drawing* after = h.project().document.find_drawing(drawing);
  REQUIRE(after != nullptr);
  CHECK(after->views[0].volumes == std::vector<Id>{base});
  REQUIRE(after->dimensions.size() == 1);
  CHECK(after->dimensions[0].to.target == base);
  CHECK(model::check(h.project().document).empty());
  h.undo();
  CHECK(h.project().document.find_drawing(drawing)->dimensions.size() == 2);
}

TEST_CASE("The dock lists drawings, views and dimensions", "[app][drawing]") {
  model::Project p = samples::load("bracket.vsproj");
  const Id base = volume(p.document, "Base");
  model::Drawing d = model::make_drawing("Sheet");
  model::DrawingView v;
  v.id = Id::random();
  v.name = "Top";
  v.direction = model::ViewDirection::Top;
  v.scale = 0.5;
  d.views.push_back(v);
  d.dimensions.push_back({Id::random(), v.id, DimensionKind::Horizontal,
                          {AnchorOn::VolumeBoundary, base, AnchorPart::Min, {}, 0},
                          {AnchorOn::VolumeContents, base, AnchorPart::Max, {}, 0}, 10});
  d.dimensions.push_back({Id::random(), v.id, DimensionKind::Diameter,
                          {AnchorOn::Surface, surface(p.document, "Flange"), AnchorPart::Hole, {}, 1},
                          {},
                          10});
  p.document.drawings.push_back(d);
  const auto rows = app::drawing_rows(p.document);
  REQUIRE(rows.size() == 4);
  CHECK(rows[0].kind == app::DrawingRowKind::Drawing);
  CHECK(rows[0].label == "Sheet");
  CHECK(rows[1].kind == app::DrawingRowKind::View);
  CHECK(rows[1].label == "Top (top, 1:2)");
  CHECK(rows[1].drawing == d.id);
  CHECK(rows[2].depth == 2);
  CHECK(rows[2].label == "Horizontal: Base boundary min to (Base contents max)");
  CHECK(rows[2].disposable);
  CHECK(rows[3].label == "Diameter: Flange hole 2");
  CHECK_FALSE(rows[3].disposable);
}

TEST_CASE("A view offers stable anchors first, then disposable ones", "[app][drawing]") {
  const model::Project p = samples::load("bracket.vsproj");
  const auto ev = samples::evaluate(p);
  REQUIRE(ev.ok());
  const Id base = volume(ev.document, "Base");
  const Id upright = volume(ev.document, "Upright");
  const Id flange = surface(ev.document, "Flange");

  model::DrawingView front;
  front.id = Id::random();
  front.direction = model::ViewDirection::Front;
  const auto h = app::anchor_choices(ev, front, DimensionKind::Horizontal);
  REQUIRE_FALSE(h.empty());
  CHECK(h.front().anchor.on == AnchorOn::Origin);
  // Stable first.
  const auto first_disposable =
      std::find_if(h.begin(), h.end(), [](const AnchorChoice& c) { return c.disposable; });
  CHECK(std::none_of(first_disposable, h.end(), [](const AnchorChoice& c) { return !c.disposable; }));
  CHECK(find(h, AnchorOn::VolumeBoundary, base, AnchorPart::Min) != nullptr);
  CHECK(find(h, AnchorOn::VolumeBoundary, upright, AnchorPart::Max) != nullptr);
  CHECK(find(h, AnchorOn::Surface, flange, AnchorPart::Centre) != nullptr);
  // Only boundary faces facing along the front view's horizontal (x).
  const model::VolumeBuild* bb = ev.build.find_volume(base);
  REQUIRE(bb != nullptr);
  std::size_t faces_x = 0;
  for (const auto& f : bb->boundary_faces) {
    const bool along_x = !f.cylinder && std::abs(f.plane.normal.x) > 1 - 1e-9;
    if (along_x) ++faces_x;
    CHECK((find(h, AnchorOn::VolumeBoundary, base, AnchorPart::Face, f.name) != nullptr) == along_x);
  }
  CHECK(faces_x == 2);
  const auto* cmax = find(h, AnchorOn::VolumeContents, base, AnchorPart::Max);
  REQUIRE(cmax != nullptr);
  CHECK(cmax->disposable);
  CHECK(cmax->label == "(Base contents max)");

  // Vertical: the parametric fill's start and end faces lie across z.
  const auto v = app::anchor_choices(ev, front, DimensionKind::Vertical);
  const auto fill_faces = std::count_if(v.begin(), v.end(), [&](const AnchorChoice& c) {
    return c.anchor.on == AnchorOn::VolumeContents && c.anchor.part == AnchorPart::Face;
  });
  CHECK(fill_faces >= 2);
  for (const auto& c : v) {
    if (c.anchor.on == AnchorOn::VolumeContents && c.anchor.part == AnchorPart::Face) {
      CHECK(c.label.starts_with("(Base contents face "));
    }
  }

  // Diameters seen from the top: only the flange's bolt holes are circles.
  model::DrawingView top = front;
  top.direction = model::ViewDirection::Top;
  const auto dia = app::anchor_choices(ev, top, DimensionKind::Diameter);
  REQUIRE_FALSE(dia.empty());
  for (const auto& c : dia) {
    CHECK(c.anchor.on != AnchorOn::Origin);
    CHECK(c.anchor.part != AnchorPart::Min);
  }
  CHECK(find(dia, AnchorOn::Surface, flange, AnchorPart::Hole) != nullptr);
  // Seen from the front, the holes are edge-on.
  CHECK(find(app::anchor_choices(ev, front, DimensionKind::Diameter), AnchorOn::Surface, flange,
             AnchorPart::Hole) == nullptr);

  // A view of the base alone offers nothing of the upright.
  front.volumes = {base};
  const auto only = app::anchor_choices(ev, front, DimensionKind::Horizontal);
  CHECK(find(only, AnchorOn::VolumeBoundary, upright, AnchorPart::Min) == nullptr);
  CHECK(find(only, AnchorOn::Surface, flange, AnchorPart::Centre) != nullptr);  // joins the base

  // Every offered anchor draws (dimensions between the first and each).
  model::Drawing d = model::make_drawing("All");
  d.views = {front, top};
  for (const auto& c : only) {
    d.dimensions.push_back({Id::random(), front.id, DimensionKind::Horizontal, only.front().anchor, c.anchor, 10});
  }
  for (const auto& c : dia) d.dimensions.push_back({Id::random(), top.id, DimensionKind::Diameter, c.anchor, {}, 10});
  draw::DrawOptions o;
  o.settings = core::ProductSettings::defaults();
  const auto r = draw::draw(ev, d, o);
  INFO(samples::codes(r.diagnostics));
  CHECK(r.ok());
  CHECK(std::all_of(r.dimensions.begin(), r.dimensions.end(), [](const auto& x) { return x.drawn; }));
}

// M16's rules, headless: faces and edges kept as pickable parts of the
// scene and named as the reference lists name them, dimension handles and
// the edits dragging them makes, swapping contents without touching
// anything outside the volume, and the guidance CAD habits get.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <set>
#include <variant>

#include "authoring.hpp"
#include "dimension_drag.hpp"
#include "guidance.hpp"
#include "history.hpp"
#include "picking.hpp"
#include "reference_choices.hpp"
#include "samples.hpp"
#include "scene.hpp"
#include "sketch_editor.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/model/check.hpp"
#include "vs/model/project.hpp"
#include "vs/proc/bake.hpp"
#include "vs/proc/generator.hpp"

using namespace vs;
using core::Id;
using Catch::Matchers::WithinAbs;

namespace {

const model::Volume& volume(const model::Document& doc, const std::string& name) {
  const auto* v = doc.find_volume(name);
  REQUIRE(v != nullptr);
  return *v;
}

const feature::Feature* find_feature(const feature::FeatureTree& tree, const std::string& label) {
  for (const auto& f : tree.features()) {
    if (tree.label(f.id) == label) return &f;
  }
  return nullptr;
}

// The first choice in the list on the same surface as `ref`: what a pick of
// any face on that surface gives.
std::optional<feature::FaceRef> first_on_surface(const eval::Evaluation& ev, const Id& tree,
                                                 const app::ReferenceChoices& choices, const feature::FaceRef& ref) {
  const auto target = app::named_surface(ev, tree, ref);
  if (!target) return std::nullopt;
  for (const auto& c : choices.faces) {
    const auto s = app::named_surface(ev, tree, c.ref);
    if (s && app::same_surface(*s, *target)) return c.ref;
  }
  return std::nullopt;
}

// Every face choice is reachable from some drawn face of a pickable item,
// and resolves to the first choice on its surface; every edge between two
// named faces resolves to the pair the lists would give.
void check_resolution(const eval::Evaluation& ev, const app::Scene& scene, const Id& tree) {
  const auto choices = app::reference_choices(ev, tree);
  const auto pickable = app::pickable_items(ev, tree);
  REQUIRE_FALSE(choices.faces.empty());
  for (const auto& choice : choices.faces) {
    INFO(choice.label);
    const auto expected = first_on_surface(ev, tree, choices, choice.ref);
    REQUIRE(expected.has_value());
    bool reached = false;
    for (const auto& [subject, role] : pickable) {
      const auto* item = scene.find(subject, role);
      if (item == nullptr) continue;
      const std::size_t faces = role == app::ItemRole::ConnectionSurface ? 1 : item->mesh.faces.size();
      for (std::size_t f = 0; f < faces && !reached; ++f) {
        const auto r = app::resolve_pick(scene, ev, tree, choices, {subject, role, app::PickedPart::Kind::Face, f});
        if (r.face && *r.face == *expected) reached = true;
      }
    }
    CHECK(reached);
  }
  std::size_t edges = 0;
  for (const auto& [subject, role] : pickable) {
    const auto* item = scene.find(subject, role);
    if (item == nullptr || role == app::ItemRole::ConnectionSurface) continue;
    REQUIRE(item->mesh.edge_faces.size() == item->mesh.edges.size());
    for (std::size_t e = 0; e < item->mesh.edges.size(); ++e) {
      const auto [a, b] = item->mesh.edge_faces[e];
      if (a < 0 || b < 0 || a == b) continue;
      const auto r = app::resolve_pick(scene, ev, tree, choices, {subject, role, app::PickedPart::Kind::Edge, e});
      if (!r.edge) continue;  // e.g. both sides on one reference
      ++edges;
      const bool in_list_a = std::any_of(choices.faces.begin(), choices.faces.end(),
                                         [&](const auto& c) { return c.ref == r.edge->face; });
      CHECK(in_list_a);
      REQUIRE(r.edge->other.has_value());
      const bool in_list_b = std::any_of(choices.faces.begin(), choices.faces.end(),
                                         [&](const auto& c) { return c.ref == *r.edge->other; });
      CHECK(in_list_b);
    }
  }
  CHECK(edges > 0);
}

}  // namespace

TEST_CASE("B-rep scene items keep each face's triangles and each edge's faces", "[app][picking]") {
  const auto ev = samples::evaluate(samples::load("bracket.vsproj"));
  const auto scene = app::build_scene(ev, core::ProductSettings::defaults());
  std::size_t checked = 0;
  for (const auto& item : scene.items) {
    if (item.role == app::ItemRole::Sketch) continue;
    if (item.mesh.faces.empty()) continue;  // meshes have none
    INFO(app::to_string(item.role));
    std::uint32_t next = 0;
    for (const auto& f : item.mesh.faces) {
      CHECK(f.first == next);  // contiguous, in order
      next = f.first + f.count;
      CHECK_FALSE(f.face.is_empty());
    }
    CHECK(next == item.mesh.triangle_count());
    REQUIRE(item.mesh.edge_faces.size() == item.mesh.edges.size());
    for (const auto& [a, b] : item.mesh.edge_faces) {
      CHECK(a >= 0);
      CHECK(a < static_cast<int>(item.mesh.faces.size()));
      CHECK(b < static_cast<int>(item.mesh.faces.size()));
    }
    ++checked;
  }
  CHECK(checked >= 3);  // two boundaries, the base's contents, the flange
}

TEST_CASE("A picked face or edge names what the reference lists name", "[app][picking][acceptance]") {
  const model::Project project = samples::load("bracket.vsproj");
  const auto ev = samples::evaluate(project);
  const auto scene = app::build_scene(ev, core::ProductSettings::defaults());
  SECTION("in the layout") { check_resolution(ev, scene, Id{}); }
  SECTION("in context") { check_resolution(ev, scene, volume(project.document, "Base").id); }
  SECTION("only what edit in context allows can be picked") {
    const Id base = volume(project.document, "Base").id;
    const Id upright = volume(project.document, "Upright").id;
    const auto in_base = app::pickable_items(ev, base);
    CHECK(std::find(in_base.begin(), in_base.end(), std::make_pair(base, app::ItemRole::Contents)) != in_base.end());
    CHECK(std::none_of(in_base.begin(), in_base.end(), [&](const auto& p) { return p.first == upright; }));
    // Generated or imported meshes have no faces to pick.
    const auto pev = samples::evaluate(samples::load("bolt_bosses.vsproj"));
    const auto pscene = app::build_scene(pev, core::ProductSettings::defaults());
    const Id bosses = volume(pev.document, "Bosses").id;
    const auto* contents = pscene.find(bosses, app::ItemRole::Contents);
    REQUIRE(contents != nullptr);
    CHECK(contents->mesh.faces.empty());
    const auto r = app::resolve_pick(pscene, pev, bosses, app::reference_choices(pev, bosses),
                                     {bosses, app::ItemRole::Contents, app::PickedPart::Kind::Face, 0});
    CHECK_FALSE(r.ok());
    CHECK_FALSE(r.why.empty());
  }
}

TEST_CASE("A Bezier's curved side and its edges can be picked", "[app][picking][bezier]") {
  // The bracket plus a "D" in the layout, away from it: a line and a Bézier
  // bulging to +x, extruded 10 mm.
  model::Project project = samples::load("bracket.vsproj");
  sketch::Sketch d(Id::random(), "D");
  const Id bottom = d.add_point({300, 0});
  const Id top = d.add_point({300, 40});
  d.add_line(bottom, top);
  const Id curve = d.add_bezier(top, d.add_point({340, 40}), d.add_point({340, 0}), bottom);
  auto& layout = project.document.layout;
  const Id sk = layout.add(feature::SketchFeature{d, feature::PrincipalPlane::XY});
  feature::ExtrudeFeature e;
  e.sketch = sk;
  e.depth = feature::Param::of(10);
  const Id body = layout.add(e);
  REQUIRE_FALSE(model::check(project.document).has_errors());

  const auto ev = samples::evaluate(project);
  const auto scene = app::build_scene(ev, core::ProductSettings::defaults());
  check_resolution(ev, scene, Id{});

  // A fillet's edges: the curved side against either cap or the flat side.
  // (A cap may be named by an earlier reference on its plane, as any pick.)
  const feature::FaceRef side{body, "side:" + curve.to_string()};
  const auto choices = app::reference_choices(ev, Id{});
  const auto* item = scene.find(body, app::ItemRole::LayoutSolid);
  REQUIRE(item != nullptr);
  bool face_picked = false;
  std::size_t edges_picked = 0;
  for (std::size_t f = 0; f < item->mesh.faces.size(); ++f) {
    const auto r = app::resolve_pick(scene, ev, Id{}, choices, {body, app::ItemRole::LayoutSolid,
                                                               app::PickedPart::Kind::Face, f});
    if (r.face && *r.face == side) {
      face_picked = true;
      CHECK_FALSE(r.plane);  // not a plane to sketch on
    }
  }
  for (std::size_t k = 0; k < item->mesh.edges.size(); ++k) {
    const auto r = app::resolve_pick(scene, ev, Id{}, choices, {body, app::ItemRole::LayoutSolid,
                                                               app::PickedPart::Kind::Edge, k});
    if (r.edge && r.edge->other && (r.edge->face == side || *r.edge->other == side)) ++edges_picked;
  }
  CHECK(face_picked);
  CHECK(edges_picked >= 3);  // the curve along the start, along the end, and a joint with the line's side
}

TEST_CASE("A sketch dimension's handle sits on what it sizes and points the way it grows", "[app][drag]") {
  sketch::Sketch sk(Id::random(), "S");
  const Id line = sk.add_line(core::dvec2{0, 0}, core::dvec2{30, 0});
  const Id len = sk.length(line, 30);
  const Id circle = sk.add_circle(core::dvec2{50, 0}, 5);
  const Id dia = sk.diameter(circle, 10);
  const auto h = app::sketch_dimension_handle(sk, *sk.find_constraint(len));
  REQUIRE(h);
  CHECK_THAT(h->anchor.x, WithinAbs(15, 1e-9));
  CHECK_THAT(h->direction.x, WithinAbs(1, 1e-9));
  const auto d = app::sketch_dimension_handle(sk, *sk.find_constraint(dia));
  REQUIRE(d);
  CHECK(d->rate == 2.0);
  CHECK_THAT(std::hypot(d->anchor.x - 50, d->anchor.y), WithinAbs(5, 1e-9));
}

TEST_CASE("Dragging the bracket's dimensions drags the variables that drive them", "[app][drag]") {
  model::Project project = samples::load("bracket.vsproj");
  const auto ev = samples::evaluate(project);
  const auto& layout = project.document.layout;
  const auto* block = find_feature(layout, "Base block");
  REQUIRE(block != nullptr);
  const auto handles = app::dimension_handles(ev, {}, block->id);
  // The depth (base_h) and the outline's width and depth.
  REQUIRE(handles.size() == 3);
  const auto depth = std::find_if(handles.begin(), handles.end(),
                                  [](const auto& h) { return h.label.find("depth (base_h)") != std::string::npos; });
  REQUIRE(depth != handles.end());
  CHECK(depth->target == app::DimensionHandle::Target::Variable);
  CHECK_THAT(depth->value, WithinAbs(10, 1e-9));
  CHECK_THAT(depth->anchor.z, WithinAbs(10, 1e-6));  // on the top of the base
  CHECK_THAT(depth->direction.z, WithinAbs(1, 1e-9));
  for (const auto& h : handles) CHECK(h.target == app::DimensionHandle::Target::Variable);

  // 4 mm up the handle: base_h is 14 and the base's boundary follows.
  const double value = app::dragged_value(*depth, 4.0);
  CHECK_THAT(value, WithinAbs(14, 1e-9));
  app::History history(project);
  REQUIRE_FALSE(history.apply(app::drag_command(*depth, value, core::ProductSettings::defaults())).has_errors());
  CHECK(history.undo_label() == "Drag " + depth->label);
  CHECK(history.project().document.layout.variables().find("base_h")->expression == "14 mm");
  const auto after = samples::evaluate(history.project());
  const auto* vb = after.build.find_volume(volume(after.document, "Base").id);
  REQUIRE(vb != nullptr);
  CHECK_THAT(kernel::bounding_box(vb->boundary).max.z, WithinAbs(14, 1e-6));
  // Never below the smallest value.
  CHECK(app::dragged_value(*depth, -100.0) > 0.0);
}

TEST_CASE("A plain sketch dimension drags its own value, solved", "[app][drag]") {
  model::Project project;
  project.document = model::make_document("Plate");
  sketch::Sketch sk(Id::random(), "Plate");
  const Id a = sk.add_point({0, 0});
  const Id b = sk.add_point({30, 0});
  const Id line = sk.add_line(a, b);
  sk.fix(a);
  sk.horizontal(line);
  const Id len = sk.length(line, 30);
  feature::Feature f;
  f.id = sk.id();
  f.definition = feature::SketchFeature{sk, feature::PrincipalPlane::XY};
  app::History history(project);
  REQUIRE_FALSE(history.apply(app::add_feature({}, f)).has_errors());
  const auto ev = samples::evaluate(history.project());
  const auto handles = app::dimension_handles(ev, {}, f.id);
  REQUIRE(handles.size() == 1);
  CHECK(handles[0].target == app::DimensionHandle::Target::SketchDimension);
  REQUIRE_FALSE(
      history.apply(app::drag_command(handles[0], app::dragged_value(handles[0], 5.0), core::ProductSettings::defaults()))
          .has_errors());
  const auto& dragged = history.project().document.layout.find(f.id)->as<feature::SketchFeature>()->sketch;
  CHECK_THAT(dragged.find_constraint(len)->dimension->value, WithinAbs(35, 1e-9));
  CHECK_THAT(dragged.point(b)->x, WithinAbs(35, 1e-6));  // solved: the free end moved
  CHECK_THAT(dragged.point(a)->x, WithinAbs(0, 1e-9));   // the fixed one didn't
}

TEST_CASE("Sketch mode drags plain dimensions and refuses ones an expression drives", "[app][drag]") {
  sketch::Sketch sk(Id::random(), "S");
  const Id a = sk.add_point({0, 0});
  const Id b = sk.add_point({20, 0});
  const Id line = sk.add_line(a, b);
  sk.fix(a);
  sk.horizontal(line);
  const Id len = sk.length(line, 20);
  app::SketchEditor ed(sk, core::ProductSettings::defaults(), [](std::string_view n) -> std::optional<double> {
    if (n == "w") return 25.0;
    return std::nullopt;
  });
  CHECK(ed.drag_dimension(len, 22, true).empty());
  CHECK(ed.drag_dimension(len, 24, false).empty());  // the same drag: one undo step
  CHECK_THAT(ed.sketch().point(b)->x, WithinAbs(24, 1e-6));
  ed.undo();
  CHECK_THAT(ed.sketch().point(b)->x, WithinAbs(20, 1e-6));
  REQUIRE(ed.set_dimension(len, 0, "w").empty());
  const auto refused = ed.drag_dimension(len, 30);
  REQUIRE(refused.size() == 1);
  CHECK(refused.items().front().code == "app.sketch.dimension_driven");
  CHECK_THAT(ed.sketch().point(b)->x, WithinAbs(25, 1e-6));
}

TEST_CASE("Swapping a volume's contents leaves every other volume's cache key unchanged",
          "[app][swap][acceptance]") {
  const model::Project start = samples::load("bracket.vsproj");
  const Id upright = volume(start.document, "Upright").id;
  const auto keys_of = [](const eval::Evaluation& ev) {
    std::map<Id, std::optional<core::Hash256>> out;
    for (const auto& r : ev.volumes) out[r.volume] = r.key;
    return out;
  };
  // The document with the swapped volume's contents blanked: what must
  // never change.
  const auto outside = [&](const model::Project& p) {
    model::Document copy = p.document;
    copy.find_volume(upright)->contents = model::StaticContents{};
    return model::to_json(copy);
  };
  const auto before = keys_of(samples::evaluate(start));
  const std::string outside_before = outside(start);

  app::History history(start);
  const auto check_others = [&](const std::string& step) {
    INFO(step);
    CHECK(outside(history.project()) == outside_before);
    const auto ev = samples::evaluate(history.project());
    const auto keys = keys_of(ev);
    for (const auto& [id, key] : before) {
      if (id == upright) continue;
      CHECK(keys.at(id) == key);
    }
    return ev;
  };
  using K = app::ContentsKind;
  REQUIRE_FALSE(history.apply(app::swap_contents(upright, K::Placeholder)).has_errors());
  CHECK(app::contents_kind(*history.project().document.find_volume(upright)) == K::Placeholder);
  check_others("placeholder");
  REQUIRE_FALSE(history.apply(app::swap_contents(upright, K::Procedural)).has_errors());
  const auto procedural = check_others("procedural");
  const auto* r = procedural.find(upright);
  REQUIRE(r != nullptr);
  const mesh::Mesh* m = proc::contents_mesh(*r);
  REQUIRE(m != nullptr);
  // Baking needs the procedure's mesh.
  CHECK(history.apply(app::swap_contents(upright, K::Baked)).has_errors());
  REQUIRE_FALSE(history.apply(app::swap_contents(upright, K::Baked, *m)).has_errors());
  CHECK(app::contents_kind(*history.project().document.find_volume(upright)) == K::Baked);
  check_others("baked");
  REQUIRE_FALSE(history.apply(app::swap_contents(upright, K::Procedural)).has_errors());  // unbaked
  check_others("unbaked");
  REQUIRE_FALSE(history.apply(app::swap_contents(upright, K::Parametric)).has_errors());
  CHECK(app::contents_kind(*history.project().document.find_volume(upright)) == K::Parametric);
  check_others("modelled");
  // Back through every step to the very same project.
  while (history.undo()) {
  }
  CHECK(model::content_hash(history.project()) == model::content_hash(start));
}

TEST_CASE("CAD habits on generated contents get the right action", "[app][guidance]") {
  const model::Project bosses_project = samples::load("bolt_bosses.vsproj");
  const auto& pdoc = bosses_project.document;
  const Id bosses = volume(pdoc, "Bosses").id;
  using A = app::Guidance::Action;
  using H = app::Habit;
  const auto action = [](const model::Document& d, const Id& v, H h, const Id& context = {}) {
    const auto g = app::guide(d, v, h, context);
    REQUIRE(g.has_value());
    CHECK_FALSE(g->title.empty());
    CHECK_FALSE(g->text.empty());
    CHECK_FALSE(g->action_label.empty());
    return g->action;
  };
  CHECK(action(pdoc, bosses, H::Fillet) == A::EditGraph);
  CHECK(action(pdoc, bosses, H::Cut) == A::AddKeepOut);
  CHECK(action(pdoc, bosses, H::Mate) == A::UseConnection);
  CHECK(app::guide(pdoc, bosses, H::Mate)->target == pdoc.connections.at(0).id);

  const model::Project bracket = samples::load("bracket.vsproj");
  const auto& bdoc = bracket.document;
  const Id base = volume(bdoc, "Base").id;
  const Id upright = volume(bdoc, "Upright").id;
  // Modelled contents: fillet inside the volume; already in context, fine.
  CHECK(action(bdoc, base, H::Fillet) == A::EditInContext);
  CHECK_FALSE(app::guide(bdoc, base, H::Fillet, base).has_value());
  CHECK(action(bdoc, base, H::Cut) == A::AddKeepOut);
  // Imported contents.
  CHECK(action(bdoc, upright, H::Fillet) == A::EditInContext);
  CHECK(action(bdoc, upright, H::Mate) == A::UseConnection);

  // Baked contents: unbake.
  app::History history(bosses_project);
  const auto ev = samples::evaluate(bosses_project);
  const mesh::Mesh* m = proc::contents_mesh(*ev.find(bosses));
  REQUIRE(m != nullptr);
  REQUIRE_FALSE(history.apply(app::swap_contents(bosses, app::ContentsKind::Baked, *m)).has_errors());
  CHECK(action(history.project().document, bosses, H::Fillet) == A::Unbake);

  // A volume no surface joins: make one.
  model::Document lonely = pdoc;
  lonely.connections.clear();
  CHECK(action(lonely, bosses, H::Mate) == A::AddConnection);
}

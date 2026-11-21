// Authoring commands and the reference choices the feature dialogs offer,
// in the layout and inside a volume (edit in context).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <variant>

#include "authoring.hpp"
#include "graph_edit.hpp"
#include "history.hpp"
#include "reference_choices.hpp"
#include "samples.hpp"
#include "vs/model/hierarchy.hpp"
#include "vs/model/references.hpp"

using namespace vs;
using core::Id;
using app::History;

namespace {

template <typename T>
bool has_label(const std::vector<app::Choice<T>>& choices, const std::string& label) {
  return std::any_of(choices.begin(), choices.end(), [&](const auto& c) { return c.label == label; });
}

// Every id a plane, axis, point or face reference names.
template <typename Ref>
std::vector<Id> ids_of(const Ref& ref) {
  std::vector<Id> out;
  std::visit(
      [&](const auto& r) {
        using T = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<T, feature::FeatureRef>) out.push_back(r.feature);
        if constexpr (std::is_same_v<T, feature::FaceRef>) out.push_back(r.feature);
        if constexpr (std::is_same_v<T, feature::SketchEntityRef>) {
          out.push_back(r.sketch);
          out.push_back(r.entity);
        }
      },
      ref);
  return out;
}

}  // namespace

TEST_CASE("Layout choices: principal planes, features above, and their faces", "[app][authoring]") {
  const auto project = samples::load("bracket.vsproj");
  const auto ev = samples::evaluate(project);
  REQUIRE(ev.ok());
  const auto c = app::reference_choices(ev, {});
  CHECK(has_label(c.planes, "XY plane"));
  CHECK(has_label(c.axes, "Z axis"));
  CHECK(c.sketches.size() == 3);
  CHECK(has_label(c.planes, "Top of base"));
  CHECK(has_label(c.faces, "Base block end"));
  CHECK(has_label(c.planes, "Base block end"));
  CHECK(c.solid_features.size() == 2);
  CHECK(std::find(c.variables.begin(), c.variables.end(), "upright_h") != c.variables.end());
  // Sides are named by their sketch curve.
  CHECK(std::any_of(c.faces.begin(), c.faces.end(),
                    [](const auto& f) { return f.label.rfind("Base block side Line ", 0) == 0; }));

  // A feature at index 1 (after the base sketch) sees only that sketch.
  const auto first = app::reference_choices(ev, {}, 1);
  CHECK(first.sketches.size() == 1);
  CHECK(first.faces.empty());
  CHECK(first.solid_features.empty());

  CHECK(c.label_of(feature::PlaneRef{feature::PrincipalPlane::YZ}) == "YZ plane");
  CHECK(c.label_of(feature::PlaneRef{feature::FeatureRef{Id::random()}}).empty());
}

TEST_CASE("Inside a volume only its boundary and connection surfaces can be referenced",
          "[app][authoring][acceptance]") {
  const auto project = samples::load("bracket.vsproj");
  const auto ev = samples::evaluate(project);
  REQUIRE(ev.ok());
  const model::Document& doc = ev.document;
  const Id base = doc.find_volume("Base")->id;
  const auto c = app::reference_choices(ev, base);

  CHECK_FALSE(has_label(c.planes, "XY plane"));
  CHECK(c.axes.empty() == false);  // the fill outline's lines
  CHECK(has_label(c.planes, "Surface: Flange"));
  CHECK(has_label(c.faces, "Surface: Flange patch"));
  CHECK(has_label(c.faces, "Boundary: Base block end"));
  CHECK(has_label(c.faces, "Boundary: Base block start"));
  CHECK(std::find(c.variables.begin(), c.variables.end(), "thickness") != c.variables.end());
  CHECK(std::find(c.variables.begin(), c.variables.end(), "width") == c.variables.end());

  // Nothing offered breaks encapsulation.
  const model::Scope scope = model::Scope::inside(base);
  auto check_ids = [&](const std::vector<Id>& ids) {
    for (const auto& id : ids) {
      const auto r = model::resolve(doc, scope, id);
      INFO(id.to_string());
      CHECK(r.ok());
      if (r) {
        CHECK(r->kind != model::TargetKind::LayoutFeature);
        CHECK(r->kind != model::TargetKind::LayoutVariable);
      }
    }
  };
  for (const auto& p : c.planes) check_ids(ids_of(p.ref));
  for (const auto& a : c.axes) check_ids(ids_of(a.ref));
  for (const auto& p : c.points) check_ids(ids_of(p.ref));
  for (const auto& f : c.faces) check_ids({f.ref.feature});

  // The upright (static) has no tree; its choices are its inputs alone.
  const Id upright = doc.find_volume("Upright")->id;
  const auto u = app::reference_choices(ev, upright);
  CHECK(app::tree_of(doc, upright) == nullptr);
  CHECK(has_label(u.planes, "Surface: Flange"));
  CHECK(u.sketches.empty());
}

TEST_CASE("Feature commands add, edit, move, suppress and delete", "[app][authoring]") {
  History h(samples::load("bracket.vsproj"));
  const Id base = h.project().document.find_volume("Base")->id;
  const std::size_t n = h.project().document.volumes.at(0).tree()->features().size();

  feature::Feature plane;
  plane.name = "Mid plane";
  plane.definition = feature::DatumPlaneFeature{
      feature::PlaneOffset{feature::FeatureRef{h.project().document.connections.at(0).id}, feature::Param::of(-5)}};
  REQUIRE_FALSE(h.apply(app::add_feature(base, plane, 0)).has_errors());
  const auto* tree = h.project().document.find_volume(base)->tree();
  REQUIRE(tree->features().size() == n + 1);
  const Id mid = tree->features().front().id;
  CHECK(tree->label(mid) == "Mid plane");
  CHECK(h.undo_label() == "Add Mid plane");

  REQUIRE_FALSE(h.apply(app::move_feature(base, mid, 5)).has_errors());
  CHECK(h.project().document.find_volume(base)->tree()->features().back().id == mid);
  REQUIRE_FALSE(h.apply(app::set_suppressed(base, mid, true)).has_errors());
  CHECK(h.project().document.find_volume(base)->tree()->find(mid)->suppressed);
  REQUIRE_FALSE(h.apply(app::rename_feature(base, mid, "Centre")).has_errors());
  CHECK(h.project().document.find_volume(base)->tree()->label(mid) == "Centre");
  REQUIRE_FALSE(h.apply(app::remove_feature(base, mid)).has_errors());
  CHECK(h.project().document.find_volume(base)->tree()->find(mid) == nullptr);
  CHECK(h.apply(app::remove_feature(base, mid)).has_errors());  // gone already

  // A layout plane referenced from inside a volume is refused (encapsulation).
  feature::Feature bad = plane;
  bad.id = {};
  const Id layout_plane = h.project().document.layout.features().at(2).id;
  bad.definition = feature::DatumPlaneFeature{
      feature::PlaneOffset{feature::FeatureRef{layout_plane}, feature::Param::of(1)}};
  const auto refused = h.apply(app::add_feature(base, bad));
  CHECK_FALSE(refused.with_code("model.reference.parent").empty());

  // A static volume has no tree.
  const Id upright = h.project().document.find_volume("Upright")->id;
  CHECK_FALSE(h.apply(app::add_feature(upright, plane)).with_code("app.edit.no_tree").empty());
}

TEST_CASE("Variable commands check names, loops and renames", "[app][authoring]") {
  History h(samples::load("bracket.vsproj"));
  REQUIRE_FALSE(h.apply(app::add_variable({}, "rib", "3 mm")).has_errors());
  CHECK(h.apply(app::add_variable({}, "rib", "4 mm")).has_errors());       // duplicate
  CHECK(h.apply(app::add_variable({}, "2bad", "4 mm")).has_errors());      // bad name
  const Id rib = h.project().document.layout.variables().find("rib")->id;
  CHECK_FALSE(h.apply(app::set_variable({}, rib, "rib", "width / 10")).has_errors());
  CHECK(h.apply(app::set_variable({}, rib, "rib", "rib + 1")).has_errors());  // loop
  CHECK_FALSE(h.apply(app::set_variable({}, rib, "rib_w", "width / 10")).has_errors());
  CHECK(h.project().document.layout.variables().find("rib_w") != nullptr);
  // Renaming one others use breaks them: refused.
  const Id width = h.project().document.layout.variables().find("width")->id;
  CHECK(h.apply(app::set_variable({}, width, "w", "60 mm")).has_errors());
  REQUIRE_FALSE(h.apply(app::remove_variable({}, rib)).has_errors());
  CHECK(h.project().document.layout.variables().find("rib_w") == nullptr);
}

TEST_CASE("Volume commands: keep-outs, contents kinds, removal cleans up", "[app][authoring]") {
  History h(samples::load("bracket.vsproj"));
  const model::Document& d0 = h.project().document;
  const Id base = d0.find_volume("Base")->id;
  const Id upright = d0.find_volume("Upright")->id;

  // A volume with contents, or joined by a surface, can't become a keep-out.
  CHECK_FALSE(h.apply(app::set_keep_out(base, true)).with_code("app.edit.keep_out_contents").empty());
  REQUIRE_FALSE(h.apply(app::make_placeholder(upright)).has_errors());
  CHECK(h.project().document.find_volume(upright)->contents_empty());
  CHECK_FALSE(h.apply(app::set_keep_out(upright, true)).with_code("app.edit.keep_out_connected").empty());

  // A new box-free volume from the upright block, made parametric.
  const Id upright_block = h.project().document.layout.features().at(4).id;
  model::Volume extra = app::volume_from_features(
      app::unique_name("Upright", {"Base", "Upright"}), {upright_block});
  CHECK(extra.name == "Upright 2");
  REQUIRE_FALSE(h.apply(app::add_volume(extra)).has_errors());
  REQUIRE_FALSE(h.apply(app::make_parametric(extra.id)).has_errors());
  CHECK(h.project().document.find_volume(extra.id)->kind() == model::VolumeKind::Parametric);
  REQUIRE_FALSE(h.apply(app::set_keep_out(extra.id, true)).has_errors());
  CHECK(h.project().document.find_volume(extra.id)->keep_out);

  // Removing the base drops the link from its mass and its side of the flange.
  REQUIRE_FALSE(h.apply(app::remove_volume(base)).has_errors());
  const model::Document& d1 = h.project().document;
  CHECK(d1.links.empty());
  REQUIRE(d1.connections.size() == 1);
  CHECK(d1.connections[0].volumes == std::vector<Id>{upright});
}

TEST_CASE("Import contents fills a placeholder from a file", "[app][authoring]") {
  History h(samples::load("bracket.vsproj"));
  const Id upright = h.project().document.find_volume("Upright")->id;
  REQUIRE_FALSE(h.apply(app::make_placeholder(upright)).has_errors());
  // The STEP file the sample holds.
  const auto& blob = *h.project().blobs.begin()->second;
  const auto d = h.apply(app::import_contents(upright, blob, "step", core::ProductSettings::defaults()));
  INFO(samples::codes(d));
  REQUIRE_FALSE(d.has_errors());
  const auto* sc = std::get_if<model::StaticContents>(&h.project().document.find_volume(upright)->contents);
  REQUIRE(sc != nullptr);
  CHECK(sc->format == "step");
  CHECK(h.project().blobs.count(sc->blob) == 1);
}

TEST_CASE("Clearing a composite's contents is refused while it has children (H5)", "[app][authoring]") {
  History h(samples::load("nested.vsproj"));
  const auto& doc = h.project().document;
  const model::Volume* composite = nullptr;
  for (const auto& v : doc.volumes) {
    if (v.composite() && !model::children(doc, v.id).empty()) composite = &v;
  }
  REQUIRE(composite != nullptr);
  const Id id = composite->id;
  const auto before = model::content_hash(h.project());
  const auto d = h.apply(app::make_placeholder(id));
  CHECK(d.with_code("app.swap.has_children").size() == 1);
  CHECK(model::content_hash(h.project()) == before);
  CHECK_FALSE(h.can_undo());

  // A leaf clears as before.
  const model::Volume* leaf = nullptr;
  for (const auto& v : h.project().document.volumes) {
    if (!v.composite() && !v.keep_out) leaf = &v;
  }
  REQUIRE(leaf != nullptr);
  const auto cleared = h.apply(app::make_placeholder(leaf->id));
  INFO(samples::codes(cleared));
  CHECK_FALSE(cleared.has_errors());
  CHECK(h.can_undo());
}

TEST_CASE("A wire between two same-named ports runs the way it was dragged (H5)", "[app][graph]") {
  const app::GraphContext none;
  proc::Graph g = app::starter_graph();
  auto c = app::add_node(g, "field.shell", none);
  REQUIRE(c.ok());
  g = c.graph;
  const Id first = c.node;
  c = app::add_node(g, "field.shell", none);
  REQUIRE(c.ok());
  g = c.graph;
  const Id second = c.node;

  // Dragged from the second shell's "field" input to the first's output:
  // the first feeds the second.
  c = app::connect_ports(g, {second, "field"}, {first, "field"}, false);
  REQUIRE(c.ok());
  const proc::Edge* e = c.graph.edge_into(second, "field");
  REQUIRE(e != nullptr);
  CHECK(e->from.node == first);
  CHECK(c.graph.edge_into(first, "field") == nullptr);

  // Dragged from the second's output: the second feeds the first.
  c = app::connect_ports(g, {second, "field"}, {first, "field"}, true);
  REQUIRE(c.ok());
  e = c.graph.edge_into(first, "field");
  REQUIRE(e != nullptr);
  CHECK(e->from.node == second);
  CHECK(c.graph.edge_into(second, "field") == nullptr);

  // Without the gesture, the first end is the output.
  c = app::connect_ports(g, {second, "field"}, {first, "field"});
  REQUIRE(c.ok());
  REQUIRE(c.graph.edge_into(first, "field") != nullptr);
  CHECK(c.graph.edge_into(first, "field")->from.node == second);
}

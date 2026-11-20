// L3: composite volumes in the app. A composite is built through the same
// commands as the root (its layout's features, child volumes, a surface
// between them, a delegation); the outline nests; reference choices and
// dialogs work at the composite's level; edit in context on a composite
// keeps its children undimmed.

#include <QApplication>
#include <QTreeWidget>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "authoring.hpp"
#include "history.hpp"
#include "main_window.hpp"
#include "model_dialogs.hpp"
#include "nested.hpp"
#include "outline.hpp"
#include "reference_choices.hpp"
#include "samples.hpp"
#include "viewport.hpp"
#include "vs/model/check.hpp"
#include "vs/model/hierarchy.hpp"
#include "vs/model/project.hpp"

using namespace vs;
using app::History;
using core::Id;
using feature::FeatureRef;
using feature::Param;

namespace {

Id point(History& h, const Id& volume, Param x, Param y, Param z, const std::string& name) {
  feature::Feature f;
  f.id = Id::random();
  f.name = name;
  f.definition = feature::DatumPointFeature{feature::PointAt{std::move(x), std::move(y), std::move(z)}};
  REQUIRE_FALSE(h.apply(app::add_feature(volume, f)).has_errors());
  return f.id;
}

Id point(History& h, const Id& volume, double x, double y, double z, const std::string& name) {
  return point(h, volume, Param::of(x), Param::of(y), Param::of(z), name);
}

template <typename T>
bool has_label(const std::vector<app::Choice<T>>& choices, const std::string& label) {
  return std::any_of(choices.begin(), choices.end(), [&](const auto& c) { return c.label == label; });
}

}  // namespace

TEST_CASE("A composite is built, undone and redone through commands", "[app][authoring][nested]") {
  History h(model::Project{model::make_document("Built"), {}});
  const auto initial = model::content_hash(h.project());
  std::vector<core::Hash256> hashes{initial};
  const auto step = [&] { hashes.push_back(model::content_hash(h.project())); };

  // The root: a base and a housing, bonded across a seat.
  REQUIRE_FALSE(h.apply(app::add_variable({}, "w", "40")).has_errors());
  step();
  const Id lo = point(h, {}, 0, 0, -10, "Base low");
  const Id origin = point(h, {}, 0, 0, 0, "Origin");
  const Id base_hi = point(h, {}, Param::expr("w"), Param::of(20), Param::of(0), "Base high");
  const Id top = point(h, {}, Param::expr("w"), Param::of(20), Param::of(20), "Top");
  step();
  const model::Volume base = app::volume_from_box("Base", FeatureRef{lo}, FeatureRef{base_hi});
  const model::Volume housing = app::volume_from_box("Housing", FeatureRef{origin}, FeatureRef{top});
  REQUIRE_FALSE(h.apply(app::add_volume(base)).has_errors());
  REQUIRE_FALSE(h.apply(app::add_volume(housing)).has_errors());
  step();
  model::ConnectionSurface seat;
  seat.id = Id::random();
  seat.name = "Seat";
  model::DatumPatch patch;
  patch.u = Param::of(10);
  patch.v = Param::of(10);
  patch.width = Param::of(20);
  patch.height = Param::of(20);
  seat.geometry = patch;
  seat.volumes = {base.id, housing.id};
  REQUIRE_FALSE(h.apply(app::add_connection(seat)).has_errors());
  step();

  // Housing becomes a composite, and its layout gets points and a plane.
  REQUIRE_FALSE(h.apply(app::swap_contents(housing.id, app::ContentsKind::Composite)).has_errors());
  CHECK(app::contents_kind(*h.project().document.find_volume(housing.id)) == app::ContentsKind::Composite);
  step();
  const Id h_origin = point(h, housing.id, 0, 0, 0, "Origin");
  const Id h_split = point(h, housing.id, 20, 20, 20, "Split");
  const Id h_right_lo = point(h, housing.id, 20, 0, 0, "Right low");
  const Id h_far = point(h, housing.id, 40, 20, 20, "Far");
  feature::Feature plane;
  plane.id = Id::random();
  plane.name = "Split plane";
  plane.definition = feature::DatumPlaneFeature{feature::PlaneOffset{feature::PrincipalPlane::YZ, Param::of(20)}};
  REQUIRE_FALSE(h.apply(app::add_feature(housing.id, plane)).has_errors());
  step();

  // Its children, a surface between them, and the seat passed to Left.
  const model::Volume left = app::volume_from_box("Left", FeatureRef{h_origin}, FeatureRef{h_split}, housing.id);
  const model::Volume right = app::volume_from_box("Right", FeatureRef{h_right_lo}, FeatureRef{h_far}, housing.id);
  REQUIRE_FALSE(h.apply(app::add_volume(left)).has_errors());
  REQUIRE_FALSE(h.apply(app::add_volume(right)).has_errors());
  step();
  model::ConnectionSurface middle;
  middle.id = Id::random();
  middle.name = "Middle";
  model::DatumPatch mp;
  mp.plane = FeatureRef{plane.id};
  mp.u = Param::of(10);
  mp.v = Param::of(10);
  mp.width = Param::of(20);
  mp.height = Param::of(20);
  middle.geometry = mp;
  middle.volumes = {left.id, right.id};
  REQUIRE_FALSE(h.apply(app::add_connection(middle)).has_errors());
  REQUIRE_FALSE(h.apply(app::set_delegation(housing.id, seat.id, left.id)).has_errors());
  step();

  const model::Document& doc = h.project().document;
  const auto findings = model::check(doc);
  INFO(samples::codes(findings));
  CHECK_FALSE(findings.has_errors());
  CHECK(model::joins(doc, *doc.find_connection(seat.id), left.id));

  // A child can't use the root's points; the root can't use the composite's.
  {
    model::Volume bad = left;
    bad.id = Id::random();
    bad.name = "Bad";
    bad.boundary = model::BoxBoundary{FeatureRef{origin}, FeatureRef{h_split}};
    CHECK_FALSE(h.apply(app::add_volume(bad)).with_code("model.reference.parent").empty());
    bad.parent = {};
    bad.boundary = model::BoxBoundary{FeatureRef{origin}, FeatureRef{h_split}};
    CHECK_FALSE(h.apply(app::add_volume(bad)).with_code("model.reference.contents").empty());
  }

  // The evaluation: the outline nests, and choices are the level's.
  const auto ev = samples::evaluate(h.project());
  INFO(samples::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  const auto rows = app::volume_rows(ev);
  REQUIRE(rows.size() == 4);
  CHECK(rows[0].name == "Base");
  CHECK(rows[1].name == "Housing");
  CHECK(rows[1].kind == model::VolumeKind::Composite);
  CHECK(app::kind_label(rows[1]) == "Composite");
  CHECK(rows[2].name == "Left");
  CHECK(rows[2].parent == housing.id);
  CHECK(rows[2].depth == 1);
  const auto inside = app::layout_choices(ev, housing.id);
  CHECK(has_label(inside.points, "Split"));
  CHECK_FALSE(has_label(inside.points, "Top"));
  CHECK(has_label(inside.planes, "Surface: Seat"));
  CHECK(has_label(inside.planes, "Split plane"));
  const auto root = app::layout_choices(ev);
  CHECK(has_label(root.points, "Top"));
  CHECK_FALSE(has_label(root.points, "Split"));

  // Swapping a composite with children away is refused.
  CHECK_FALSE(h.apply(app::swap_contents(housing.id, app::ContentsKind::Placeholder))
                  .with_code("app.swap.has_children")
                  .empty());

  // Undo every step back to the start, then redo: identical hashes.
  for (std::size_t k = hashes.size() - 1; k > 0; --k) {
    while (model::content_hash(h.project()) != hashes[k - 1]) REQUIRE(h.undo());
  }
  CHECK(model::content_hash(h.project()) == initial);
  while (h.can_redo()) REQUIRE(h.redo());
  CHECK(model::content_hash(h.project()) == hashes.back());

  // Deleting the composite takes its children, their surface and the
  // delegation with it; the seat keeps the base.
  REQUIRE_FALSE(h.apply(app::remove_volume(housing.id)).has_errors());
  const model::Document& after = h.project().document;
  CHECK(after.volumes.size() == 1);
  REQUIRE(after.connections.size() == 1);
  CHECK(after.connections[0].volumes == std::vector<Id>{base.id});
  CHECK_FALSE(model::check(after).has_errors());
}

TEST_CASE("Editing a composite in context works at its level", "[app][gui][authoring][nested]") {
  // The nested document, saved and opened as a user would.
  auto n = nested::make_nested();
  nested::fill_leaves(n);
  const auto dir = std::filesystem::temp_directory_path() / ("vstudio-nested-" + Id::random().to_string() + ".vsproj");
  REQUIRE_FALSE(model::save_project(n.project, dir, model::ProjectLayout::Folder).has_errors());

  app::MainWindow w;
  w.resize(1280, 800);
  w.show();
  REQUIRE(w.open_project(dir));
  REQUIRE(samples::wait_until(
      [&] {
        const auto s = w.session().latest();
        return !w.session().busy() && s && !s->preview && s->project == w.session().project_ptr();
      },
      120000));
  QApplication::processEvents();

  // The volume tree nests: Core under Left under Housing.
  const auto find_item = [&](const QString& name) -> QTreeWidgetItem* {
    const auto items = w.volume_tree().findItems(name, Qt::MatchExactly | Qt::MatchRecursive);
    return items.isEmpty() ? nullptr : items.front();
  };
  QTreeWidgetItem* core = find_item(QStringLiteral("Core"));
  REQUIRE(core != nullptr);
  REQUIRE(core->parent() != nullptr);
  CHECK(core->parent()->text(0) == QStringLiteral("Left"));
  REQUIRE(core->parent()->parent() != nullptr);
  CHECK(core->parent()->parent()->text(0) == QStringLiteral("Housing"));

  // In Housing's layout: Base dims, its children don't.
  REQUIRE(w.edit_in_context(n.housing));
  CHECK(w.authoring_level() == n.housing);
  CHECK(w.viewport().dimmed_items() > 0);
  QTreeWidgetItem* base = find_item(QStringLiteral("Base"));
  QTreeWidgetItem* right = find_item(QStringLiteral("Right"));
  REQUIRE(base != nullptr);
  REQUIRE(right != nullptr);
  CHECK(base->foreground(0).color() == QColor(150, 150, 155));
  CHECK_FALSE(right->foreground(0).color() == QColor(150, 150, 155));

  // A new volume goes in Housing, with Housing's points to choose from.
  auto* d = w.new_volume();
  REQUIRE(d != nullptr);
  CHECK(d->value().parent == n.housing);
  d->reject();
  samples::wait_until([&] { return w.open_dialog() == nullptr; }, 5000);
  // A new connection surface picks between Housing's children only.
  auto* c = w.new_connection();
  REQUIRE(c != nullptr);
  c->reject();
  samples::wait_until([&] { return w.open_dialog() == nullptr; }, 5000);

  // Passing the seat to Right instead, then back.
  REQUIRE(w.set_met_by(n.housing, n.seat, n.right));
  const auto* comp = std::get_if<model::CompositeContents>(&w.history().project().document.find_volume(n.housing)->contents);
  REQUIRE(comp != nullptr);
  CHECK(comp->delegation(n.seat)->child == n.right);
  // Left no longer receives it, so its own pass to Core went with it.
  const auto* left_comp =
      std::get_if<model::CompositeContents>(&w.history().project().document.find_volume(n.left)->contents);
  CHECK(left_comp->delegation(n.seat) == nullptr);
  w.undo();
  comp = std::get_if<model::CompositeContents>(&w.history().project().document.find_volume(n.housing)->contents);
  CHECK(comp->delegation(n.seat)->child == n.left);

  w.leave_context();
  CHECK(w.authoring_level().is_null());
  std::error_code ignored;
  std::filesystem::remove_all(dir, ignored);
}

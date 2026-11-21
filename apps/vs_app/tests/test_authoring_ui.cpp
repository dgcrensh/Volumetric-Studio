// Authoring in the main window (M14): live previews in feature dialogs,
// sketch mode, edit in context, and the acceptance test: the bracket
// rebuilt from scratch entirely through the app's own entry points
// (dialogs, sketch mode, the variables table), then every step undone and
// redone with identical document hashes.

#include <QApplication>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <variant>

#include "authoring.hpp"
#include "feature_dialog.hpp"
#include "main_window.hpp"
#include "model_dialogs.hpp"
#include "reference_choices.hpp"
#include "samples.hpp"
#include "sketch_mode.hpp"
#include "viewport.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/model/build.hpp"
#include "vs/validate/validate.hpp"
#include "widgets.hpp"
#include "wait.hpp"

using namespace vs;
using core::Id;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using sketch::ConstraintKind;

namespace {

// Waits until the session has evaluated the project as it stands now.
void settle(app::MainWindow& w) {
  REQUIRE(samples::wait_until(
      [&] {
        const auto s = w.session().latest();
        return !w.session().busy() && s && !s->preview && s->project == w.session().project_ptr();
      },
      120000));
  QApplication::processEvents();
}

const feature::Feature* find_feature(const feature::FeatureTree& tree, const std::string& label) {
  for (const auto& f : tree.features()) {
    if (tree.label(f.id) == label) return &f;
  }
  return nullptr;
}

// Draws a rectangle from `lo` to `hi` in sketch mode, fixes its first
// corner and dimensions its width and height (values or expressions).
void rectangle(app::SketchMode& mode, core::dvec2 lo, core::dvec2 hi, const QString& width, const QString& height) {
  auto& ed = mode.editor();
  mode.set_tool(app::SketchTool::Rectangle);
  ed.click(lo);
  REQUIRE(ed.click(hi).size() == 8);
  mode.set_tool(app::SketchTool::Select);
  mode.refresh();
  const Id corner = ed.nearest_point(lo, 0.01);
  REQUIRE_FALSE(corner.is_null());
  // Bottom edge, then the right edge, picked as a user would (near them).
  const Id bottom = ed.nearest_entity({(lo.x + hi.x) / 2, lo.y}, 0.5);
  const Id right = ed.nearest_entity({hi.x, (lo.y + hi.y) / 2}, 0.5);
  REQUIRE_FALSE(bottom.is_null());
  REQUIRE_FALSE(right.is_null());
  mode.view().set_selection({corner});
  REQUIRE(mode.constrain(ConstraintKind::Fix));
  mode.view().set_selection({bottom});
  REQUIRE(mode.constrain(ConstraintKind::Distance));
  const Id w_dim = mode.view().selection().front();
  REQUIRE(mode.set_dimension_text(w_dim, width));
  mode.view().set_selection({right});
  REQUIRE(mode.constrain(ConstraintKind::Distance));
  const Id h_dim = mode.view().selection().front();
  REQUIRE(mode.set_dimension_text(h_dim, height));
  INFO(mode.status_text().toStdString());
  REQUIRE(ed.status() == sketch::SolveStatus::Solved);
}

// A feature dialog, filled from `f`: the dialog must express it exactly.
void fill(app::FeatureDialog& d, const feature::Feature& f) {
  d.set_feature(f);
  const feature::Feature back = d.feature();
  CHECK(back == f);
  REQUIRE(d.complete());
}

void commit(app::MainWindow& w, app::EditDialog* d) {
  REQUIRE(d != nullptr);
  const auto before = w.history().undo_count();
  d->commit();
  INFO(d->message().toStdString());
  REQUIRE(w.history().undo_count() == before + 1);
  CHECK(w.open_dialog() == nullptr);
  settle(w);
}

}  // namespace

TEST_CASE("A feature dialog previews each change and commits one undoable step", "[app][gui][authoring]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj"));
  settle(w);
  const auto h0 = model::content_hash(w.history().project());
  const auto* block = find_feature(w.history().project().document.layout, "Upright block");
  REQUIRE(block != nullptr);
  const Id block_id = block->id;

  // Typing a new depth previews it: the session shows it, the history doesn't have it.
  auto* d = w.edit_feature({}, block_id);
  REQUIRE(d != nullptr);
  auto edits = d->findChildren<app::ParamEdit*>();
  REQUIRE_FALSE(edits.empty());
  edits.front()->setText(QStringLiteral("upright_h + 15"));  // the depth
  w.flush_preview();
  CHECK(w.previewing());
  settle(w);
  auto shown = w.session().project().document.layout.find(block_id)->as<feature::ExtrudeFeature>()->depth;
  CHECK(shown.expression == "upright_h + 15");
  CHECK(model::content_hash(w.history().project()) == h0);
  {
    // The preview reached the viewport: the upright's boundary is 55 tall.
    const auto s = w.session().latest();
    const auto* vb = s->evaluation.build.find_volume(s->evaluation.document.find_volume("Upright")->id);
    REQUIRE(vb != nullptr);
    CHECK_THAT(kernel::bounding_box(vb->boundary).max.z, WithinAbs(65.0, 1e-6));
  }

  // Cancel: the project is back.
  d->reject();
  settle(w);
  CHECK_FALSE(w.previewing());
  CHECK(model::content_hash(w.session().project()) == h0);

  // Again, committed: one step, undone to the same hash.
  d = w.edit_feature({}, block_id);
  d->findChildren<app::ParamEdit*>().front()->setText(QStringLiteral("55"));
  commit(w, d);
  CHECK(w.history().undo_label() == "Edit Upright block");
  CHECK(w.history().project().document.layout.find(block_id)->as<feature::ExtrudeFeature>()->depth.value == 55.0);
  w.undo();
  settle(w);
  CHECK(model::content_hash(w.history().project()) == h0);
  CHECK(model::content_hash(w.session().project()) == h0);
  w.redo();
  settle(w);
  CHECK(w.history().project().document.layout.find(block_id)->as<feature::ExtrudeFeature>()->depth.value == 55.0);

  // An edit the model refuses keeps the dialog open with the reason.
  auto* bad = w.edit_feature({}, block_id);
  feature::Feature f = *w.history().project().document.layout.find(block_id);
  f.as<feature::ExtrudeFeature>()->depth = feature::Param::expr("no_such_variable");
  bad->set_feature(f);
  const auto steps = w.history().undo_count();
  bad->commit();
  // The rebuild reports unknown names; the model accepts the edit (it is
  // the feature that fails), so this commits and shows the failure.
  settle(w);
  CHECK(w.history().undo_count() == steps + 1);
  const auto rows = app::feature_rows(w.session().latest()->evaluation, {});
  CHECK(std::any_of(rows.begin(), rows.end(), [](const auto& r) { return r.status == "failed"; }));
}

TEST_CASE("Sketch mode draws, constrains and names conflicts, then commits one step", "[app][gui][authoring]") {
  app::MainWindow w;
  w.show();
  w.new_project("Sketching");
  settle(w);
  app::SketchMode* mode = w.new_sketch(feature::PrincipalPlane::XY, "Plate");
  REQUIRE(mode != nullptr);
  REQUIRE(w.sketch_mode() == mode);
  rectangle(*mode, {0, 0}, {30, 20}, QStringLiteral("30"), QStringLiteral("20"));
  CHECK(mode->status_text() == QStringLiteral("Fully constrained"));
  CHECK(mode->constraint_list().topLevelItemCount() == 7);

  // A conflicting dimension: named in the status and drawn red in the list.
  auto& ed = mode->editor();
  const Id top = ed.nearest_entity({15, 20}, 0.5);
  mode->view().set_selection({top});
  REQUIRE(mode->constrain(ConstraintKind::Distance));
  const Id clash = mode->view().selection().front();
  REQUIRE(mode->set_dimension_text(clash, QStringLiteral("35")));
  CHECK(mode->status_text().startsWith(QStringLiteral("Conflicting constraints: ")));
  CHECK(mode->status_text().contains(QString::fromStdString(ed.sketch().label(clash))));
  bool red = false;
  for (int i = 0; i < mode->constraint_list().topLevelItemCount(); ++i) {
    auto* item = mode->constraint_list().topLevelItem(i);
    if (item->text(0) == QString::fromStdString(ed.sketch().label(clash))) {
      red = item->foreground(0).color() == QColor(210, 40, 40);
    }
  }
  CHECK(red);
  // Undo in sketch mode undoes the sketch's own steps.
  w.undo();
  w.undo();
  CHECK(ed.status() == sketch::SolveStatus::Solved);
  CHECK(w.history().undo_count() == 0);

  mode->finish(true);
  settle(w);
  CHECK(w.sketch_mode() == nullptr);
  REQUIRE(w.history().undo_count() == 1);
  CHECK(w.history().undo_label() == "Add Plate");
  const auto& layout = w.history().project().document.layout;
  REQUIRE(layout.features().size() == 1);
  CHECK(layout.features()[0].as<feature::SketchFeature>()->sketch.constraints().size() == 7);
  // The unused sketch is drawn in the viewport.
  CHECK(w.viewport().shown_items(app::ItemRole::Sketch) == 1);

  // Editing it again and cancelling changes nothing.
  mode = w.edit_sketch({}, layout.features()[0].id);
  REQUIRE(mode != nullptr);
  mode->editor().remove({mode->editor().sketch().constraints().front().id});
  mode->finish(false);
  CHECK(w.history().undo_count() == 1);
}

TEST_CASE("Editing a volume in context dims the rest and offers only its inputs", "[app][gui][authoring][acceptance]") {
  app::MainWindow w;
  w.resize(1280, 800);
  w.show();
  REQUIRE(w.open_project(samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj"));
  settle(w);
  const auto& doc = w.history().project().document;
  const Id base = doc.find_volume("Base")->id;
  const Id upright = doc.find_volume("Upright")->id;
  CHECK_FALSE(w.edit_in_context(upright));  // static contents: clear them first

  REQUIRE(w.edit_in_context(base));
  CHECK(w.editing_volume() == base);
  CHECK(w.viewport().context_volume() == base);
  CHECK(w.viewport().dimmed_items() > 0);
  // The feature panel shows the contents; the variables panel their variables.
  CHECK(w.feature_tree().topLevelItemCount() == 2);

  // A new datum plane: its plane choices are the boundary and the flange.
  auto* d = w.insert_feature(feature::FeatureKind::DatumPlane);
  REQUIRE(d != nullptr);
  QStringList planes;
  for (auto* box : d->findChildren<QComboBox*>()) {
    for (int i = 0; i < box->count(); ++i) planes << box->itemText(i);
  }
  CHECK(planes.contains(QStringLiteral("Surface: Flange")));
  CHECK(planes.contains(QStringLiteral("Boundary: Base block end")));
  CHECK_FALSE(planes.contains(QStringLiteral("XY plane")));
  CHECK_FALSE(planes.contains(QStringLiteral("Top of base")));
  // Added in the contents' tree.
  feature::Feature f = d->feature();
  f.definition = feature::DatumPlaneFeature{
      feature::PlaneOffset{feature::FeatureRef{doc.connections.at(0).id}, feature::Param::of(-2)}};
  fill(*d, f);
  commit(w, d);
  CHECK(w.history().project().document.find_volume(base)->tree()->features().size() == 3);
  CHECK(w.history().project().document.layout.features().size() == 6);

  w.leave_context();
  CHECK(w.editing_volume().is_null());
  CHECK(w.viewport().dimmed_items() == 0);
  const QImage image = w.viewport().render_image();
  CHECK_FALSE(image.isNull());
}

TEST_CASE("The bracket can be rebuilt from scratch entirely in the app", "[app][gui][authoring][acceptance]") {
  const model::Project sample = samples::load("bracket.vsproj");
  app::MainWindow w;
  w.resize(1440, 900);
  w.show();
  w.new_project("Bracket");
  settle(w);
  std::vector<core::Hash256> hashes{model::content_hash(w.history().project())};
  QObject::connect(&w, &app::MainWindow::historyChanged, [&] {
    // Only forward steps (undo and redo are checked against these later).
    if (w.history().undo_count() == hashes.size()) hashes.push_back(model::content_hash(w.history().project()));
  });
  auto doc = [&]() -> const model::Document& { return w.history().project().document; };

  // --- variables, through the variables table
  {
    auto buttons = w.findChildren<QPushButton*>();
    auto add = std::find_if(buttons.begin(), buttons.end(),
                            [](auto* b) { return b->text() == QStringLiteral("Add variable"); });
    REQUIRE(add != buttons.end());
    const std::pair<const char*, const char*> vars[] = {
        {"width", "60 mm"}, {"depth", "40 mm"}, {"base_h", "10 mm"}, {"upright_t", "10 mm"}, {"upright_h", "40 mm"}};
    for (const auto& [name, expression] : vars) {
      (*add)->click();
      settle(w);
      auto& table = w.variables_table();
      const int row = table.rowCount() - 1;
      table.item(row, 1)->setText(QString::fromUtf8(expression));
      table.item(row, 0)->setText(QString::fromUtf8(name));
      settle(w);
    }
    REQUIRE(doc().layout.variables().variables().size() == 5);
    CHECK(doc().layout.variables().find("upright_h")->expression == "40 mm");
  }

  // --- layout: base outline and block
  {
    auto* d = w.insert_feature(feature::FeatureKind::Sketch);
    feature::Feature f = d->feature();
    f.name = "Base outline";
    f.as<feature::SketchFeature>()->plane = feature::PrincipalPlane::XY;
    fill(*d, f);
    d->commit();
    app::SketchMode* mode = w.sketch_mode();
    REQUIRE(mode != nullptr);
    rectangle(*mode, {0, 0}, {60, 40}, QStringLiteral("width"), QStringLiteral("depth"));
    mode->finish(true);
    settle(w);
  }
  const Id base_sketch = doc().layout.features().back().id;
  auto extrude = [&](const std::string& name, const Id& sketch, const std::string& depth, bool reverse = false) {
    auto* d = w.insert_feature(feature::FeatureKind::Extrude);
    feature::Feature f = d->feature();
    f.name = name;
    auto* e = f.as<feature::ExtrudeFeature>();
    e->sketch = sketch;
    e->depth = feature::Param::expr(depth);
    e->reverse = reverse;
    fill(*d, f);
    commit(w, d);
    return doc().layout.features().back().id;
  };
  const Id base_block = extrude("Base block", base_sketch, "base_h");

  // --- the top of the base, and the upright on it
  {
    auto* d = w.insert_feature(feature::FeatureKind::DatumPlane);
    feature::Feature f = d->feature();
    f.name = "Top of base";
    f.definition = feature::DatumPlaneFeature{
        feature::PlaneOffset{feature::PrincipalPlane::XY, feature::Param::expr("base_h")}};
    fill(*d, f);
    commit(w, d);
  }
  const Id top = doc().layout.features().back().id;
  auto sketch_on = [&](const feature::PlaneRef& plane, const std::string& name, core::dvec2 hi, const QString& wd,
                       const QString& ht) {
    auto* d = w.insert_feature(feature::FeatureKind::Sketch);
    feature::Feature f = d->feature();
    f.name = name;
    f.as<feature::SketchFeature>()->plane = plane;
    fill(*d, f);
    d->commit();
    app::SketchMode* mode = w.sketch_mode();
    REQUIRE(mode != nullptr);
    rectangle(*mode, {0, 0}, hi, wd, ht);
    mode->finish(true);
    settle(w);
    const auto* tree = app::tree_of(doc(), w.editing_volume());
    return tree->features().back().id;
  };
  const Id upright_sketch =
      sketch_on(feature::FeatureRef{top}, "Upright outline", {60, 10}, QStringLiteral("width"), QStringLiteral("upright_t"));
  const Id upright_block = extrude("Upright block", upright_sketch, "upright_h");
  const Id flange_sketch =
      sketch_on(feature::FeatureRef{top}, "Flange outline", {60, 10}, QStringLiteral("width"), QStringLiteral("upright_t"));
  // Blocked out but not yet volumes: both solids are drawn as layout solids.
  CHECK(w.viewport().shown_items(app::ItemRole::LayoutSolid) == 2);

  // --- volumes
  {
    auto* d = w.new_volume();
    REQUIRE(d != nullptr);
    model::Volume v = d->value();
    v.name = "Base";
    v.boundary = model::FeatureBoundary{{base_block}};
    v.seed = 42;
    v.parameters = {{Id::random(), "thickness", feature::Param::expr("base_h")}};
    v.outputs = {{Id::random(), "mass", model::OutputQuantity::Mass, 1.24},
                 {Id::random(), "volume", model::OutputQuantity::Volume, 1.0}};
    d->set_value(v);
    CHECK(d->value() == v);
    commit(w, d);
  }
  {
    auto* d = w.new_volume();
    model::Volume v = d->value();
    v.name = "Upright";
    v.boundary = model::FeatureBoundary{{upright_block}};
    v.parameters = {{Id::random(), "load", feature::Param::of(0)}};
    v.outputs = {{Id::random(), "envelope", model::OutputQuantity::Envelope, 1.0}};
    d->set_value(v);
    CHECK(d->value() == v);
    commit(w, d);
  }
  CHECK(w.viewport().shown_items(app::ItemRole::LayoutSolid) == 0);
  const Id base = doc().find_volume("Base")->id;
  const Id upright = doc().find_volume("Upright")->id;

  // --- the bolted flange between them
  {
    auto* d = w.new_connection();
    REQUIRE(d != nullptr);
    model::ConnectionSurface s = d->value();
    s.name = "Flange";
    s.geometry = model::SketchPatch{flange_sketch};
    s.volumes = {base, upright};
    model::BoltedFlangeParams bolts;
    model::BoltGrid grid;
    grid.columns = feature::Param::of(2);
    grid.rows = feature::Param::of(1);
    grid.pitch_x = feature::Param::of(40);
    bolts.holes.layout = grid;
    bolts.holes.u = feature::Param::expr("width / 2");
    bolts.holes.v = feature::Param::expr("upright_t / 2");
    bolts.thread = "M4";
    bolts.min_wall = feature::Param::of(2);
    s.params = bolts;
    d->set_value(s);
    CHECK(d->value() == s);
    commit(w, d);
  }
  const Id flange = doc().connections.at(0).id;

  // --- the base filled by modelling, in context
  REQUIRE(w.edit_in_context(base));
  settle(w);
  const Id fill_sketch =
      sketch_on(feature::FeatureRef{flange}, "Fill outline", {60, 40}, QStringLiteral("60"), QStringLiteral("40"));
  {
    auto* d = w.insert_feature(feature::FeatureKind::Extrude);
    feature::Feature f = d->feature();
    f.name = "Fill";
    auto* e = f.as<feature::ExtrudeFeature>();
    e->sketch = fill_sketch;
    e->depth = feature::Param::expr("thickness");
    e->reverse = true;
    fill(*d, f);
    commit(w, d);
  }
  w.leave_context();

  // --- the upright's contents: the sample's STEP file, imported
  {
    const auto& blob = *sample.blobs.begin()->second;
    const auto r = w.apply(app::import_contents(upright, blob, "step", core::ProductSettings::defaults()));
    INFO(samples::codes(r));
    REQUIRE_FALSE(r.has_errors());
    settle(w);
  }

  // --- the base's mass drives the upright's load
  {
    auto* d = w.new_link();
    REQUIRE(d != nullptr);
    model::Link l = d->value();
    const auto& b = *doc().find_volume(base);
    l.source = {base, b.find_output("mass")->id};
    l.target = doc().find_volume(upright)->find_parameter("load")->id;
    d->set_value(l);
    CHECK(d->value() == l);
    commit(w, d);
  }

  // --- the result builds and validates like the sample
  settle(w);
  const auto built = w.session().latest();
  INFO(samples::codes(built->evaluation.diagnostics));
  REQUIRE(built->evaluation.ok());
  const auto expected = samples::evaluate(sample);
  REQUIRE(expected.ok());
  const model::Document& mine = built->evaluation.document;
  const model::Document& theirs = expected.document;
  CHECK(mine.layout.features().size() == theirs.layout.features().size());
  for (std::size_t i = 0; i < theirs.layout.features().size(); ++i) {
    CHECK(mine.layout.features()[i].kind() == theirs.layout.features()[i].kind());
    CHECK(mine.layout.label(mine.layout.features()[i].id) == theirs.layout.label(theirs.layout.features()[i].id));
  }
  for (const auto& var : theirs.layout.variables().variables()) {
    REQUIRE(mine.layout.variables().find(var.name) != nullptr);
    CHECK(mine.layout.variables().find(var.name)->expression == var.expression);
  }
  REQUIRE(mine.volumes.size() == theirs.volumes.size());
  for (const auto& tv : theirs.volumes) {
    INFO(tv.name);
    const auto* mv = mine.find_volume(tv.name);
    REQUIRE(mv != nullptr);
    CHECK(mv->kind() == tv.kind());
    CHECK(mv->seed == tv.seed);
    const auto bm = kernel::bounding_box(built->evaluation.build.find_volume(mv->id)->boundary);
    const auto bt = kernel::bounding_box(expected.build.find_volume(tv.id)->boundary);
    CHECK_THAT(distance(bm.min, bt.min), WithinAbs(0.0, 1e-6));
    CHECK_THAT(distance(bm.max, bt.max), WithinAbs(0.0, 1e-6));
    const auto* rm = built->evaluation.find(mv->id);
    const auto* rt = expected.find(tv.id);
    REQUIRE(rm->contents);
    REQUIRE(rt->contents);
    const auto cm = kernel::bounding_box(rm->contents->solid);
    const auto ct = kernel::bounding_box(rt->contents->solid);
    CHECK_THAT(distance(cm.min, ct.min), WithinAbs(0.0, 1e-6));
    CHECK_THAT(distance(cm.max, ct.max), WithinAbs(0.0, 1e-6));
    for (const auto& to : tv.outputs) {
      const auto* mo = mv->find_output(to.name);
      REQUIRE(mo != nullptr);
      std::optional<model::OutputValue> a, b;
      for (const auto& r : rm->contents->outputs) {
        if (r.output == mo->id) a = r.value;
      }
      for (const auto& r : rt->contents->outputs) {
        if (r.output == to.id) b = r.value;
      }
      REQUIRE(a);
      REQUIRE(b);
      if (const auto* x = std::get_if<double>(&*a)) CHECK_THAT(*x, WithinRel(std::get<double>(*b), 1e-9));
    }
  }
  const auto* cm = built->evaluation.build.find_connection(flange);
  const auto* ct = expected.build.find_connection(theirs.connections.at(0).id);
  REQUIRE(cm->ok);
  CHECK_THAT(cm->area, WithinRel(ct->area, 1e-9));
  REQUIRE(cm->hole_centres.size() == ct->hole_centres.size());
  for (std::size_t i = 0; i < ct->hole_centres.size(); ++i) {
    CHECK_THAT(distance(cm->hole_centres[i], ct->hole_centres[i]), WithinAbs(0.0, 1e-6));
  }
  REQUIRE(mine.links.size() == 1);
  const auto report = validate::validate(mine, core::ProductSettings::defaults(), &w.history().project().blobs);
  INFO(samples::codes(report.diagnostics));
  CHECK(report.ok());
  // The welded mesh exports, as the sample's does.
  CHECK(built->evaluation.combined.has_value() == expected.combined.has_value());

  // --- undo every step back to the empty project, and redo them all
  const std::size_t steps = w.history().undo_count();
  REQUIRE(hashes.size() == steps + 1);
  WARN("the bracket took " << steps << " undoable steps");
  for (std::size_t i = steps; i > 0; --i) {
    w.undo();
    CHECK(model::content_hash(w.history().project()) == hashes[i - 1]);
  }
  CHECK_FALSE(w.history().can_undo());
  settle(w);
  CHECK(model::content_hash(w.session().project()) == hashes.front());
  for (std::size_t i = 1; i <= steps; ++i) {
    w.redo();
    CHECK(model::content_hash(w.history().project()) == hashes[i]);
  }
  settle(w);
  CHECK(w.session().latest()->evaluation.ok());
}

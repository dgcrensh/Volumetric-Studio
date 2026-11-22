// The authoring dialogs and the window around them (H7): every datum
// definition can be confirmed, numbers are checked before OK, edits of one
// part of a dialog keep what was typed in the others, picking ends with its
// page, a new sketch's dialog closes once, sketch mode owns its keys,
// unsaved changes are asked about, and a node form's old fields stay quiet.

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QKeySequence>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <variant>

#include "authoring.hpp"
#include "feature_dialog.hpp"
#include "main_window.hpp"
#include "model_dialogs.hpp"
#include "node_editor.hpp"
#include "samples.hpp"
#include "sketch_mode.hpp"
#include "vs/proc/graph.hpp"
#include "wait.hpp"
#include "widgets.hpp"

using namespace vs;
using core::Id;
using namespace vs::feature;

namespace {

struct Refs {
  Id p1 = Id::random(), p2 = Id::random(), p3 = Id::random(), solid = Id::random();
  app::ReferenceChoices choices() const {
    app::ReferenceChoices c;
    c.planes = {{"XY", PrincipalPlane::XY}, {"YZ", PrincipalPlane::YZ}, {"ZX", PrincipalPlane::ZX}};
    c.axes = {{"X", PrincipalAxis::X}, {"Z", PrincipalAxis::Z}};
    c.points = {{"P1", FeatureRef{p1}}, {"P2", FeatureRef{p2}}, {"P3", FeatureRef{p3}}};
    c.faces = {{"Block start", FaceRef{solid, "start"}}, {"Block end", FaceRef{solid, "end"}}};
    return c;
  }
};

Feature make(FeatureDefinition definition) {
  Feature f;
  f.id = Id::random();
  f.definition = std::move(definition);
  return f;
}

// The dialog shows `f` exactly and lets it be confirmed.
void confirmable(const Feature& f, const app::ReferenceChoices& choices) {
  app::FeatureDialog d(f, choices);
  CHECK(d.feature() == f);
  CHECK(d.complete());
}

QComboBox* definition_combo(app::FeatureDialog& d) {
  // "Defined by" is the dialog's first combo box.
  const auto boxes = d.findChildren<QComboBox*>();
  REQUIRE_FALSE(boxes.empty());
  return boxes.front();
}

}  // namespace

TEST_CASE("Every datum definition can be confirmed in its dialog", "[app][gui][dialogs]") {
  const Refs r;
  const auto c = r.choices();
  const PointRef a = FeatureRef{r.p1}, b = FeatureRef{r.p2}, p = FeatureRef{r.p3};
  SECTION("datum planes") {
    confirmable(make(DatumPlaneFeature{PlaneOffset{PrincipalPlane::YZ, Param::of(5.0)}}), c);
    confirmable(make(DatumPlaneFeature{PlaneThroughPoints{a, b, p}}), c);
    confirmable(make(DatumPlaneFeature{PlaneAxisAngle{PrincipalAxis::X, PrincipalPlane::XY, Param::expr("tilt")}}), c);
  }
  SECTION("datum axes") {
    confirmable(make(DatumAxisFeature{AxisThroughPoints{a, b}}), c);
    confirmable(make(DatumAxisFeature{AxisPlaneIntersection{PrincipalPlane::XY, PrincipalPlane::YZ}}), c);
    confirmable(make(DatumAxisFeature{AxisAlong{PrincipalAxis::X}}), c);
  }
  SECTION("datum points") {
    confirmable(make(DatumPointFeature{PointAt{Param::of(1.0), Param::of(2.0), Param::of(3.0)}}), c);
    confirmable(make(DatumPointFeature{PointAtRef{p}}), c);
    confirmable(make(DatumPointFeature{PointAxisPlane{PrincipalAxis::Z, PrincipalPlane::ZX}}), c);
  }
  SECTION("a number on a page not shown never blocks OK") {
    app::FeatureDialog d(make(DatumPointFeature{PointAt{Param::of(1.0), Param::of(2.0), Param::of(3.0)}}), c);
    d.findChildren<app::ParamEdit*>().front()->setText(QStringLiteral("1 +"));  // X, unfinished
    CHECK_FALSE(d.complete());
    definition_combo(d)->setCurrentIndex(1);  // at a point
    CHECK(d.complete());
    CHECK(std::holds_alternative<PointAtRef>(d.feature().as<DatumPointFeature>()->definition));
  }
}

TEST_CASE("Switching a datum's definition stops picking on the page it leaves", "[app][gui][dialogs]") {
  const Refs r;
  app::FeatureDialog d(make(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(1.0)}}), r.choices());
  int last = -2;
  QObject::connect(&d, &app::FeatureDialog::pickRequested, [&](int field) { last = field; });
  d.begin_pick(0);  // the offset page's plane
  REQUIRE(d.picking() == 0);
  definition_combo(d)->setCurrentIndex(1);
  CHECK(d.picking() == -1);
  CHECK(last == -1);  // the viewport was told
}

TEST_CASE("Adding or picking edges keeps the radius as typed", "[app][gui][dialogs]") {
  const Refs r;
  for (const bool fillet : {true, false}) {
    INFO((fillet ? "fillet" : "chamfer"));
    const Feature f = fillet ? make(FilletFeature{{}, Param::of(1.0)}) : make(ChamferFeature{{}, Param::of(1.0)});
    app::FeatureDialog d(f, r.choices());
    auto* radius = d.findChildren<app::ParamEdit*>().front();
    radius->setText(QStringLiteral("2 *"));  // still being typed
    auto* add = d.findChild<QPushButton*>(QStringLiteral("add_edges"));
    REQUIRE(add != nullptr);
    add->click();
    CHECK(radius->text() == QStringLiteral("2 *"));
    // A pick in the view, as the main window delivers it.
    int edge_field = -1;
    for (int i = 0; i < d.pick_field_count(); ++i) {
      if (d.pick_kind(i) == app::FeatureDialog::PickKind::Edge) edge_field = i;
    }
    REQUIRE(edge_field >= 0);
    d.begin_pick(edge_field);
    app::PickResolution pick;
    pick.edge = EdgeRef{FaceRef{r.solid, "end"}, std::nullopt};
    pick.label = "Block end";
    REQUIRE(d.apply_pick(pick));
    CHECK(radius->text() == QStringLiteral("2 *"));
    radius->setText(QStringLiteral("2 * 1.5"));
    const Feature back = d.feature();
    const auto& edges = fillet ? back.as<FilletFeature>()->edges : back.as<ChamferFeature>()->edges;
    CHECK(edges.size() == 2);
    CHECK((fillet ? back.as<FilletFeature>()->radius : back.as<ChamferFeature>()->distance).value == 3.0);
  }
}

TEST_CASE("Volume and connection dialogs refuse numbers that don't read", "[app][gui][dialogs]") {
  const auto project = samples::load("bracket.vsproj");
  const auto ev = samples::evaluate(project);
  REQUIRE(ev.ok());
  const auto& doc = project.document;

  SECTION("a volume's seed, parameters and densities") {
    app::VolumeDialog d(doc.volumes.at(0), ev);
    REQUIRE(d.complete());
    auto* seed = d.findChildren<QLineEdit*>().at(1);  // after the name
    seed->setText(QStringLiteral("12x"));
    CHECK_FALSE(d.complete());
    seed->setText(QStringLiteral("12"));
    CHECK(d.complete());
    d.add_parameter("load", QStringLiteral("3 *"));
    CHECK_FALSE(d.complete());
    auto* table = d.findChildren<QTableWidget*>().at(0);
    table->item(table->rowCount() - 1, 1)->setText(QStringLiteral("3 * 2"));
    CHECK(d.complete());
    d.add_output("mass", model::OutputQuantity::Mass, 0.0);  // no density
    CHECK_FALSE(d.complete());
  }
  SECTION("a bolted flange's numbers") {
    REQUIRE_FALSE(doc.connections.empty());
    app::ConnectionDialog d(doc.connections.at(0), ev);
    REQUIRE(d.complete());
    // The minimum wall is the last number field.
    auto* min_wall = d.findChildren<app::ParamEdit*>().back();
    min_wall->setText(QStringLiteral("2 +"));
    CHECK_FALSE(d.complete());
    min_wall->setText(QStringLiteral("2"));
    CHECK(d.complete());
  }
}

// --- the main window (H7) -------------------------------------------------------------------

namespace {

void settle(app::MainWindow& w) {
  REQUIRE(samples::wait_until(
      [&] {
        const auto s = w.session().latest();
        return !w.session().busy() && s && !s->preview && s->project == w.session().project_ptr();
      },
      120000));
  QApplication::processEvents();
}

std::filesystem::path bracket_path() {
  return samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj";
}

QAction* menu_action(app::MainWindow& w, const QString& text) {
  for (QAction* a : w.findChildren<QAction*>()) {
    if (a->text() == text) return a;
  }
  return nullptr;
}

// Changes the first volume's seed: one undoable step, so unsaved changes.
void edit_something(app::MainWindow& w) {
  model::Volume v = w.history().project().document.volumes.at(0);
  v.seed += 1;
  REQUIRE_FALSE(w.apply(app::update_volume(v)).has_errors());
  REQUIRE(w.history().dirty());
}

}  // namespace

TEST_CASE("A new sketch's plane dialog closes once, accepted", "[app][gui][dialogs]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bracket_path()));
  settle(w);
  auto* d = w.insert_feature(feature::FeatureKind::Sketch);
  REQUIRE(d != nullptr);
  int accepted = 0, rejected = 0;
  QObject::connect(d, &QDialog::accepted, [&] { ++accepted; });
  QObject::connect(d, &QDialog::rejected, [&] { ++rejected; });
  d->commit();
  QApplication::processEvents();
  CHECK(accepted == 1);
  CHECK(rejected == 0);
  CHECK(w.open_dialog() == nullptr);
  REQUIRE(w.sketch_mode() != nullptr);
  w.sketch_mode()->finish(false);
}

TEST_CASE("Sketch mode has Ctrl+Z, Ctrl+Y, Esc and F to itself", "[app][gui][dialogs]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bracket_path()));
  settle(w);
  const auto& layout = w.history().project().document.layout;
  Id sketch;
  for (const auto& f : layout.features()) {
    if (f.kind() == FeatureKind::Sketch && sketch.is_null()) sketch = f.id;
  }
  REQUIRE_FALSE(sketch.is_null());
  const QList<QKeySequence> keys = {QKeySequence(QKeySequence::Undo), QKeySequence(QKeySequence::Redo),
                                    QKeySequence(Qt::Key_Escape), QKeySequence(Qt::Key_F)};
  // Window actions (not sketch mode's own) holding one of those keys.
  auto window_holders = [&] {
    int n = 0;
    for (QAction* a : w.findChildren<QAction*>()) {
      bool in_sketch = false;
      for (QObject* p = a; p != nullptr; p = p->parent()) in_sketch = in_sketch || p == w.sketch_mode();
      if (!in_sketch && keys.contains(a->shortcut())) ++n;
    }
    return n;
  };
  CHECK(window_holders() == 4);
  REQUIRE(w.edit_sketch({}, sketch) != nullptr);
  // Two actions with one key in reach: Qt runs neither (an ambiguous
  // shortcut), so the window's step aside while sketch mode is open.
  CHECK(window_holders() == 0);
  w.sketch_mode()->finish(false);
  // Sketch mode (and its own actions) goes by deleteLater().
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  CHECK(w.sketch_mode() == nullptr);
  CHECK(window_holders() == 4);
}

TEST_CASE("A dimension that can't be used says why and keeps saying it", "[app][gui][dialogs]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bracket_path()));
  settle(w);
  const auto& layout = w.history().project().document.layout;
  bool tried = false;
  for (const auto& f : layout.features()) {
    if (f.kind() != FeatureKind::Sketch || tried) continue;
    auto* mode = w.edit_sketch({}, f.id);
    REQUIRE(mode != nullptr);
    Id dimension;
    for (const auto& c : mode->editor().sketch().constraints()) {
      if (c.dimension && dimension.is_null()) dimension = c.id;
    }
    if (!dimension.is_null()) {
      tried = true;
      CHECK_FALSE(mode->set_dimension_text(dimension, QStringLiteral("2 *")));
      CHECK(mode->status_text().contains(QStringLiteral("Can't use")));
    }
    mode->finish(false);
    QApplication::processEvents();
  }
  CHECK(tried);
}

TEST_CASE("Unsaved changes are asked about before New, Open and Close", "[app][gui][dialogs]") {
  // A copy, so saving doesn't touch the sample.
  const auto dir = std::filesystem::temp_directory_path() / "vs_app_unsaved";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "bracket.vsproj";
  std::filesystem::copy(bracket_path(), path, std::filesystem::copy_options::recursive);
  const std::uint64_t seed = samples::load("bracket.vsproj").document.volumes.at(0).seed;
  {
    app::MainWindow w;
    w.show();
    REQUIRE(w.open_project(path));
    settle(w);
    int asked = 0;
    auto answer = app::MainWindow::UnsavedChoice::Cancel;
    w.set_unsaved_prompt([&] {
      ++asked;
      return answer;
    });
    // Nothing changed: nothing to ask.
    w.close();
    CHECK(asked == 0);
    CHECK_FALSE(w.isVisible());
    w.show();

    edit_something(w);
    const std::string name = w.history().project().document.name;
    // Cancel keeps everything as it is.
    w.close();
    CHECK(asked == 1);
    CHECK(w.isVisible());
    QAction* new_action = menu_action(w, QStringLiteral("&New"));
    REQUIRE(new_action != nullptr);
    new_action->trigger();
    CHECK(asked == 2);
    CHECK(w.history().project().document.name == name);
    QAction* open_action = menu_action(w, QStringLiteral("&Open…"));
    REQUIRE(open_action != nullptr);
    open_action->trigger();  // Cancel: no file dialog
    CHECK(asked == 3);
    CHECK(w.history().dirty());

    // Discard: New goes ahead.
    answer = app::MainWindow::UnsavedChoice::Discard;
    new_action->trigger();
    CHECK(asked == 4);
    settle(w);
    CHECK_FALSE(w.history().dirty());

    // Save: saved, then closed.
    REQUIRE(w.open_project(path));
    settle(w);
    edit_something(w);
    answer = app::MainWindow::UnsavedChoice::Save;
    w.close();
    CHECK(asked == 5);
    CHECK_FALSE(w.isVisible());
    CHECK_FALSE(w.history().dirty());
  }
  auto back = model::load_project(path);
  REQUIRE(back.ok());
  CHECK(back->document.volumes.at(0).seed == seed + 1);
  std::filesystem::remove_all(dir);
}

TEST_CASE("A node form's old fields never edit the node shown next", "[app][gui][dialogs]") {
  const auto project = samples::load("shell_and_ball.vsproj");
  const auto& volume = project.document.volumes.at(0);
  auto graph = proc::graph_from_json(std::get<model::ProceduralContents>(volume.contents).graph);
  REQUIRE(graph.ok());
  REQUIRE(graph->nodes.size() >= 2);
  const Id a = graph->nodes.at(0).id, b = graph->nodes.at(1).id;
  app::NodeForm form;
  form.show();
  std::vector<std::pair<Id, QString>> renamed;
  QObject::connect(&form, &app::NodeForm::renamed, [&](Id node, QString name) { renamed.emplace_back(node, name); });
  form.show_node(*graph, a, {});
  auto* old_name = qobject_cast<QLineEdit*>(form.field(".name"));
  REQUIRE(old_name != nullptr);
  old_name->setText(QStringLiteral("typed for a"));
  form.show_node(*graph, b, {});
  // The old field, hidden but not yet deleted, finishes editing (it lost
  // focus, say): that must not rename b.
  emit old_name->editingFinished();
  QApplication::processEvents();
  for (const auto& r : renamed) CHECK(r.first != b);
}

// L5 in the window (plan docs/plans/L5-configurations-and-design-tables.md,
// "App"): the design table dialog edits rows and columns, imports and
// exports CSV and refuses a bad table; the toolbar's Configuration box
// switches what is shown in one undoable step; the inspector says which
// numbers the configuration drives.

#include <QApplication>
#include <QComboBox>
#include <QHeaderView>
#include <QTableWidget>
#include <QTreeWidget>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "configurations.hpp"
#include "design_table_dialog.hpp"
#include "history.hpp"
#include "main_window.hpp"
#include "samples.hpp"
#include "vs/model/design_table.hpp"
#include "vs/model/project.hpp"
#include "wait.hpp"

using namespace vs;
using app::History;
using core::Id;

namespace {

Id variable(const model::Document& d, std::string_view name) {
  const auto* v = d.layout.variables().find(name);
  REQUIRE(v != nullptr);
  return v->id;
}

Id parameter(const model::Document& d, std::string_view volume, std::string_view name) {
  const model::Volume* v = d.find_volume(volume);
  REQUIRE(v != nullptr);
  const auto* p = v->find_parameter(name);
  REQUIRE(p != nullptr);
  return p->id;
}

void apply(History& h, const app::Command& c) {
  const auto d = h.apply(c);
  INFO(samples::codes(d));
  REQUIRE_FALSE(d.has_errors());
}

std::string read(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::filesystem::path cli_data() { return samples::source_dir() / "apps" / "vstudio-cli" / "tests" / "data"; }

int column_of(app::DesignTableDialog& d, const QString& label) {
  for (int c = 0; c < d.grid().columnCount(); ++c) {
    if (d.grid().horizontalHeaderItem(c)->text() == label) return c;
  }
  return -1;
}

void settle(app::MainWindow& w) {
  REQUIRE(samples::wait_until(
      [&] {
        const auto s = w.session().latest();
        return !w.session().busy() && s && !s->preview && s->project == w.session().project_ptr();
      },
      120000));
  QApplication::processEvents();
}

QString inspector_value(app::MainWindow& w, const QString& group, const QString& name) {
  QTreeWidget& t = w.inspector();
  for (int g = 0; g < t.topLevelItemCount(); ++g) {
    const QTreeWidgetItem* gi = t.topLevelItem(g);
    if (gi->text(0) != group) continue;
    for (int i = 0; i < gi->childCount(); ++i) {
      if (gi->child(i)->text(0) == name) return gi->child(i)->text(1);
    }
  }
  return {};
}

}  // namespace

TEST_CASE("The design table dialog edits rows and columns, imports CSV and refuses a bad table",
          "[app][configuration][dialogs]") {
  History h(samples::load("bracket.vsproj"));
  const Id width = variable(h.project().document, "width");
  const Id thickness = parameter(h.project().document, "Base", "thickness");
  const Id thin = Id::random();
  apply(h, app::add_configuration("Thin", {}, false, thin));
  apply(h, app::set_configuration_value(thin, thickness, "6"));
  const auto h0 = model::content_hash(h.project());

  app::DesignTableDialog d(h.project().document);
  // The grid: a name column, then the driven numbers; headers say what is
  // authored.
  REQUIRE(d.grid().rowCount() == 1);
  REQUIRE(d.grid().columnCount() == 2);
  CHECK(d.grid().item(0, 0)->text() == "Thin");
  CHECK(d.grid().horizontalHeaderItem(1)->text() == "Base.thickness");
  CHECK(d.grid().horizontalHeaderItem(1)->toolTip().contains("Authored"));
  CHECK(d.table() == app::table_for_editing(h.project().document));

  // Add column offers what isn't a column yet, in drivable order.
  const auto offered = d.addable();
  CHECK(std::any_of(offered.begin(), offered.end(), [](const model::Drivable& x) { return x.label == "width"; }));
  CHECK_FALSE(d.add_column(thickness));  // already one
  REQUIRE(d.add_column(width));
  REQUIRE(d.grid().columnCount() == 3);
  CHECK(d.grid().horizontalHeaderItem(1)->text() == "width");
  const int w_col = column_of(d, "width");

  // Rows: add, copy (a unique name, a new configuration), remove.
  d.add_row("Heavy");
  REQUIRE(d.grid().rowCount() == 2);
  d.set_cell(1, w_col, "80");
  d.copy_row(1);
  REQUIRE(d.grid().rowCount() == 3);
  CHECK(d.grid().item(2, 0)->text() == "Heavy copy");
  CHECK(d.table().rows[2].configuration.is_null());
  d.remove_row(2);
  REQUIRE(d.grid().rowCount() == 2);

  // OK's command: one undo step, ids kept by name.
  apply(h, app::set_design_table(d.table()));
  const auto& configs = h.project().document.configurations;
  REQUIRE(configs.size() == 2);
  CHECK(configs[0].id == thin);
  CHECK(configs[1].name == "Heavy");
  CHECK(configs[1].find(width)->expression == "80");
  REQUIRE(h.undo());
  CHECK(model::content_hash(h.project()) == h0);

  // Export writes what from_csv() reads back.
  const auto back = model::from_csv(h.project().document, d.csv());
  REQUIRE(back);
  REQUIRE(back->rows.size() == 2);
  CHECK(back->rows[1].name == "Heavy");

  // Import: the CLI's sample table replaces the grid.
  CHECK_FALSE(d.import_csv(cli_data() / "bracket-configurations.csv").has_errors());
  const auto imported = d.table();
  REQUIRE(imported.rows.size() == 2);
  CHECK(imported.rows[0].name == "Thin");
  CHECK(imported.rows[0].configuration == thin);  // kept by name
  CHECK(imported.rows[1].name == "Heavy");
  REQUIRE(imported.columns.size() == 2);
  for (const auto& c : imported.columns) CHECK_FALSE(c.target.is_null());
  apply(h, app::set_design_table(imported));
  CHECK(h.project().document.configurations.size() == 2);
  REQUIRE(h.undo());

  // A column naming nothing drivable doesn't import; the grid stays.
  const auto bad = d.import_csv_text(read(cli_data() / "bad-column.csv"));
  CHECK(bad.has_errors());
  CHECK(d.table() == imported);
  CHECK(d.import_csv("no such file.csv").has_errors());

  // A table the document refuses changes nothing: a value that doesn't
  // parse, two rows of one name.
  auto broken = imported;
  broken.rows[0].cells[0] = "70 +";
  CHECK(h.apply(app::set_design_table(broken)).has_errors());
  broken = imported;
  broken.rows[1].name = broken.rows[0].name;
  CHECK(h.apply(app::set_design_table(broken)).has_errors());
  CHECK(model::content_hash(h.project()) == h0);
}

TEST_CASE("The Configuration box switches what is shown, and undoes", "[app][gui][configuration]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj"));
  settle(w);
  QComboBox& box = w.configuration_box();
  CHECK(box.count() == 1);
  CHECK_FALSE(box.isEnabled());

  const model::Document& doc = w.history().project().document;
  const Id base = doc.find_volume("Base")->id;
  const Id thickness = parameter(doc, "Base", "thickness");
  const auto authored = [&] {
    const auto s = w.session().latest();
    return s->evaluation.build.find_volume(base)->parameters.at("thickness");
  }();
  REQUIRE(authored != 7.5);

  const Id thin = Id::random();
  REQUIRE_FALSE(w.apply(app::add_configuration("Thin", {}, false, thin)).has_errors());
  REQUIRE_FALSE(w.apply(app::set_configuration_value(thin, thickness, "7.5")).has_errors());
  settle(w);
  REQUIRE(box.count() == 2);
  CHECK(box.isEnabled());
  CHECK(box.currentIndex() == 0);
  CHECK(box.itemText(0) == "Authored values");
  CHECK(box.itemText(1) == "Thin");

  // Choosing Thin: one undoable step, and the evaluation shows it.
  const auto steps = w.history().undo_count();
  box.setCurrentIndex(1);
  CHECK(w.history().undo_count() == steps + 1);
  CHECK(w.history().project().document.active_configuration == thin);
  settle(w);
  {
    const auto s = w.session().latest();
    CHECK(s->evaluation.configuration == thin);
    CHECK(s->evaluation.build.find_volume(base)->parameters.at("thickness") == 7.5);
  }
  // The inspector marks the driven parameter.
  w.select(app::Selection::of_volume(base));
  const QString shown = inspector_value(w, "Parameters", "thickness");
  INFO(shown.toStdString());
  CHECK(shown.contains("(configuration: 7.5)"));

  // Undo shows the authored values again, and the box follows.
  w.undo();
  CHECK(box.currentIndex() == 0);
  CHECK(w.history().project().document.active_configuration.is_null());
  settle(w);
  {
    const auto s = w.session().latest();
    CHECK(s->evaluation.configuration.is_null());
    CHECK(s->evaluation.build.find_volume(base)->parameters.at("thickness") == authored);
  }
  w.select(app::Selection::of_volume(base));
  CHECK_FALSE(inspector_value(w, "Parameters", "thickness").contains("configuration"));
  w.redo();
  CHECK(box.currentIndex() == 1);

  // The entry point does what the box does; showing what is shown is no step.
  const auto now = w.history().undo_count();
  CHECK_FALSE(w.show_configuration(thin).has_errors());
  CHECK(w.history().undo_count() == now);
  CHECK_FALSE(w.show_configuration(Id::null()).has_errors());
  CHECK(box.currentIndex() == 0);
  settle(w);
}

TEST_CASE("Model > Design Table commits one step and keeps a refused table open",
          "[app][gui][configuration][dialogs]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj"));
  settle(w);
  const Id width = variable(w.history().project().document, "width");

  app::DesignTableDialog* d = w.open_design_table();
  REQUIRE(d != nullptr);
  CHECK(w.open_dialog() == d);
  CHECK(d->grid().rowCount() == 0);
  d->add_row("Wide");
  REQUIRE(d->add_column(width));
  d->set_cell(0, 1, "70 +");
  w.flush_preview();
  // Refused: the dialog stays open and says why; nothing was applied.
  const auto steps = w.history().undo_count();
  d->commit();
  CHECK(w.open_dialog() == d);
  CHECK_FALSE(d->message().isEmpty());
  CHECK(w.history().undo_count() == steps);

  d->set_cell(0, 1, "90");
  w.flush_preview();
  d->commit();
  CHECK(w.open_dialog() == nullptr);
  CHECK(w.history().undo_count() == steps + 1);
  const auto& configs = w.history().project().document.configurations;
  REQUIRE(configs.size() == 1);
  CHECK(configs[0].name == "Wide");
  CHECK(configs[0].find(width)->expression == "90");
  QApplication::processEvents();
  CHECK(w.configuration_box().count() == 2);
  settle(w);

  w.undo();
  CHECK(w.history().project().document.configurations.empty());
  CHECK(w.configuration_box().count() == 1);
  settle(w);
}

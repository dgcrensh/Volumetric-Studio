// The Problems drawer (U1 phase 5, decision 0039): the diagnostics live in
// a drawer under the viewport with a count in the status bar; the drawer
// opens itself only after something the person started fails, never for a
// background evaluation; the Browser's sections count rows with problems;
// the node editor shows in the Graph context only.

#include <QApplication>
#include <QDockWidget>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>

#include "browser.hpp"
#include "main_window.hpp"
#include "problems.hpp"
#include "samples.hpp"
#include "viewport.hpp"
#include "wait.hpp"

using namespace vs;
using core::Id;

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

bool inside(const QWidget* w, const QWidget* ancestor) {
  for (const QWidget* p = w; p != nullptr; p = p->parentWidget()) {
    if (p == ancestor) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("Problems are a count in the status bar and a drawer under the viewport", "[app][gui][problems]") {
  app::MainWindow w;
  w.show();
  w.resize(1440, 900);
  QApplication::processEvents();
  app::ProblemsDrawer& drawer = w.problems_drawer();
  app::ProblemsButton& button = w.problems_button();
  // The list is in the drawer, and the drawer shares a splitter with the
  // viewport: no Diagnostics dock any more.
  CHECK(inside(&w.diagnostics_list(), &drawer));
  auto* split = qobject_cast<QSplitter*>(drawer.parentWidget());
  REQUIRE(split != nullptr);
  CHECK(inside(&w.viewport(), split->widget(0)));
  for (QDockWidget* d : w.findChildren<QDockWidget*>()) {
    CHECK_FALSE(inside(&w.diagnostics_list(), d));
  }
  CHECK_FALSE(w.problems_open());
  CHECK(button.text() == QStringLiteral("No problems"));

  // A background evaluation with errors updates the count, not the drawer.
  const auto overlap = samples::source_dir() / "libs" / "vs_validate" / "tests" / "data" / "overlap.vsproj";
  REQUIRE(w.open_project(overlap));
  settle(w);
  CHECK(button.errors() + button.warnings() > 0);
  CHECK(static_cast<std::size_t>(w.diagnostics_list().topLevelItemCount()) >= button.errors() + button.warnings());
  CHECK_FALSE(w.problems_open());
  CHECK_FALSE(button.isChecked());
  // The Volumes section counts its volumes with problems.
  CHECK(w.browser().section_header(0).text().startsWith(QStringLiteral("Volumes  (")));

  // The button opens and closes the drawer; opening pushes the viewport up.
  const int full = w.viewport().height();
  button.click();
  QApplication::processEvents();
  CHECK(w.problems_open());
  CHECK(button.isChecked());
  CHECK(drawer.height() >= 120);
  CHECK(w.viewport().height() < full - 100);
  CHECK(w.viewport().height() > full / 3);
  drawer.close_button().click();
  QApplication::processEvents();
  CHECK_FALSE(w.problems_open());
  CHECK_FALSE(button.isChecked());
  CHECK(w.viewport().height() >= full - 8);

  // Validate is something the person started: it opens the drawer.
  bool done = false;
  w.validate([&](const app::ShipCheck&) { done = true; });
  REQUIRE(samples::wait_until([&] { return done; }, 120000));
  QApplication::processEvents();
  CHECK(w.problems_open());
  CHECK(drawer.summary().startsWith(QStringLiteral("Validation:")));
  w.set_problems_open(false);

  // So is opening a file that can't be read.
  CHECK_FALSE(w.open_project(std::filesystem::temp_directory_path() / "no such project.vsproj"));
  QApplication::processEvents();
  CHECK(w.problems_open());
  CHECK(drawer.summary().startsWith(QStringLiteral("Open:")));
}

TEST_CASE("A clean project opens with the drawer closed and no badges", "[app][gui][problems]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj"));
  settle(w);
  CHECK_FALSE(w.problems_open());
  CHECK(w.problems_button().errors() == 0);
  CHECK(w.browser().section_header(0).text() == QStringLiteral("Volumes"));
  CHECK(w.browser().section_header(1).text() == QStringLiteral("Features"));
}

TEST_CASE("The node editor shows in the Graph context only", "[app][gui][problems]") {
  app::MainWindow w;
  w.show();
  w.resize(1440, 900);
  REQUIRE(w.open_project(samples::source_dir() / "libs" / "vs_proc" / "tests" / "data" / "bolt_bosses.vsproj"));
  settle(w);
  CHECK(w.graph_dock().isHidden());
  CHECK_FALSE(w.graph_dock().toggleViewAction()->isVisible());
  const Id bosses = w.history().project().document.find_volume("Bosses")->id;
  REQUIRE(w.edit_graph(bosses));
  QApplication::processEvents();
  CHECK(w.graph_dock().isVisible());
  CHECK(w.context() == app::Context::Graph);
  w.leave_graph();
  QApplication::processEvents();
  CHECK(w.graph_dock().isHidden());
  // A tab click comes back to it.
  w.select(app::Selection::of_volume(bosses));
  REQUIRE(w.enter_context(app::Context::Graph));
  QApplication::processEvents();
  CHECK(w.graph_dock().isVisible());
  REQUIRE(w.enter_context(app::Context::Layout));
  QApplication::processEvents();
  CHECK(w.graph_dock().isHidden());
}

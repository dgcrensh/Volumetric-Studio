// The Browser and the folding panels (U1 phase 4, decision 0039): the left
// panels are sections of one Browser, sections fold to a line, the Browser
// and the Inspector fold to a strip and give the viewport the room, and
// sketch mode folds the Browser for as long as it is open.

#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QTreeWidgetItemIterator>
#include <QDockWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>

#include "browser.hpp"
#include "drawing_window.hpp"
#include "main_window.hpp"
#include "samples.hpp"
#include "sketch_mode.hpp"
#include "theme.hpp"
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

std::filesystem::path bracket_path() {
  return samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj";
}

bool inside(const QWidget* w, const QWidget* ancestor) {
  for (const QWidget* p = w; p != nullptr; p = p->parentWidget()) {
    if (p == ancestor) return true;
  }
  return false;
}

QWidget* dock_of(QWidget* w) {
  for (QWidget* p = w; p != nullptr; p = p->parentWidget()) {
    if (auto* d = qobject_cast<QDockWidget*>(p)) return d;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("The Browser holds the volumes, features, variables and drawings", "[app][gui][browser]") {
  app::MainWindow w;
  w.show();
  w.resize(1440, 900);
  QApplication::processEvents();
  app::Browser& b = w.browser();
  REQUIRE(b.section_count() == 4);
  CHECK(b.section_header(0).text() == QStringLiteral("Volumes"));
  CHECK(b.section_header(1).text() == QStringLiteral("Features"));
  CHECK(b.section_header(2).text() == QStringLiteral("Variables"));
  CHECK(b.section_header(3).text() == QStringLiteral("Drawings"));
  CHECK(inside(&w.volume_tree(), &b));
  CHECK(inside(&w.feature_tree(), &b));
  CHECK(inside(&w.variables_table(), &b));
  CHECK(inside(&w.drawings_tree(), &b));
  CHECK(dock_of(&b) == &w.drawings_dock());
  // One left dock: the Browser.
  int left = 0;
  for (QDockWidget* d : w.findChildren<QDockWidget*>()) {
    if (w.dockWidgetArea(d) == Qt::LeftDockWidgetArea && !d->isHidden()) ++left;
  }
  CHECK(left == 1);

  // Sections fold to their header line and open again.
  CHECK(b.section_open(0));
  CHECK_FALSE(b.section_open(3));
  CHECK_FALSE(w.drawings_tree().isVisible());
  b.section_header(0).click();
  QApplication::processEvents();
  CHECK_FALSE(b.section_open(0));
  CHECK_FALSE(w.volume_tree().isVisible());
  b.section_header(0).click();
  QApplication::processEvents();
  CHECK(w.volume_tree().isVisible());
}

TEST_CASE("The Browser and the Inspector fold to a strip and give the viewport the room", "[app][gui][browser]") {
  app::MainWindow w;
  w.show();
  w.resize(1440, 900);
  QApplication::processEvents();
  app::Browser& b = w.browser();
  QDockWidget& browser_dock = w.drawings_dock();
  const int viewport = w.viewport().width();
  const int browser_width = browser_dock.width();

  b.fold_button().click();
  QApplication::processEvents();
  CHECK(b.folded());
  CHECK_FALSE(w.volume_tree().isVisible());
  CHECK(browser_dock.width() <= app::FoldPanel::strip_width() + 8);
  CHECK(w.viewport().width() >= viewport + browser_width - app::FoldPanel::strip_width() - 16);

  // A strip button unfolds the Browser on its section.
  auto* drawings = b.findChild<QToolButton*>(QStringLiteral("strip_3"));
  REQUIRE(drawings != nullptr);
  drawings->click();
  QApplication::processEvents();
  CHECK_FALSE(b.folded());
  CHECK(b.section_open(3));
  CHECK(w.drawings_tree().isVisible());
  CHECK(std::abs(browser_dock.width() - browser_width) <= 8);

  // The Inspector folds the same way.
  app::FoldPanel& inspector = w.inspector_panel();
  const int before = w.viewport().width();
  inspector.fold_button().click();
  QApplication::processEvents();
  CHECK(inspector.folded());
  CHECK_FALSE(w.inspector().isVisible());
  CHECK(w.viewport().width() > before + 100);
  inspector.unfold_button().click();
  QApplication::processEvents();
  CHECK(w.inspector().isVisible());
}

TEST_CASE("Sketch mode folds the Browser while it is open", "[app][gui][browser]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bracket_path()));
  settle(w);
  Id sketch;
  for (const auto& f : w.history().project().document.layout.features()) {
    if (f.kind() == feature::FeatureKind::Sketch && sketch.is_null()) sketch = f.id;
  }
  REQUIRE_FALSE(sketch.is_null());
  app::Browser& b = w.browser();

  REQUIRE(w.edit_sketch({}, sketch) != nullptr);
  QApplication::processEvents();
  CHECK(b.folded());
  w.sketch_mode()->finish(false);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QApplication::processEvents();
  CHECK_FALSE(b.folded());

  // Folded before: still folded after.
  b.set_folded(true);
  REQUIRE(w.edit_sketch({}, sketch) != nullptr);
  w.sketch_mode()->finish(false);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QApplication::processEvents();
  CHECK(b.folded());
}

TEST_CASE("The side panels run the full height and the viewport gets the middle", "[app][gui][browser]") {
  app::MainWindow w;
  w.show();
  w.resize(1440, 900);
  QApplication::processEvents();
  // Nothing below holds the window taller than a 768-line screen allows.
  CHECK(w.minimumSizeHint().height() <= 720);
  CHECK(w.minimumSize().height() <= 720);
  // The Browser and the Inspector take the full height under the toolbar;
  // the diagnostics start short, so the viewport keeps most of the height.
  CHECK(w.drawings_dock().height() > w.height() / 2 + 150);
  CHECK(w.viewport().height() >= w.height() / 2);
  CHECK(w.viewport().width() >= 1440 - 2 * 300 - 40);
}

// --- U1 phase 6 --------------------------------------------------------------------------------

TEST_CASE("The panels' layout comes back from settings", "[app][gui][browser]") {
  const auto file = std::filesystem::temp_directory_path() / "vs_app_layout_test.ini";
  std::filesystem::remove(file);
  const QString path = QString::fromStdWString(file.wstring());
  int browser_width = 0;
  {
    app::MainWindow w;
    w.show();
    w.resize(1440, 900);
    QApplication::processEvents();
    w.resizeDocks({&w.drawings_dock()}, {380}, Qt::Horizontal);
    QApplication::processEvents();
    browser_width = w.drawings_dock().width();
    w.browser().set_section_open(1, false);
    w.browser().set_section_open(3, true);
    w.inspector_panel().set_folded(true);
    w.set_problems_open(true);
    QApplication::processEvents();
    QSettings settings(path, QSettings::IniFormat);
    w.save_layout(settings);
  }
  app::MainWindow w;
  w.show();
  w.resize(1440, 900);
  QApplication::processEvents();
  // The defaults first.
  CHECK(w.browser().section_open(1));
  CHECK_FALSE(w.inspector_panel().folded());
  {
    const QSettings settings(path, QSettings::IniFormat);
    w.restore_layout(settings);
  }
  QApplication::processEvents();
  CHECK(std::abs(w.drawings_dock().width() - browser_width) <= 8);
  CHECK_FALSE(w.browser().section_open(1));
  CHECK(w.browser().section_open(3));
  CHECK(w.inspector_panel().folded());
  // The drawer stays closed, but opens at the height it had.
  CHECK_FALSE(w.problems_open());
  // Unfolding the Inspector gives back its width, not the strip's.
  w.inspector_panel().unfold_button().click();
  QApplication::processEvents();
  CHECK(w.inspector().width() > 150);
  std::filesystem::remove(file);
}

TEST_CASE("A drawing shows in place of the viewport", "[app][gui][browser]") {
  app::MainWindow w;
  w.show();
  w.resize(1440, 900);
  REQUIRE(w.open_project(bracket_path()));
  settle(w);
  const QSize before = w.size();
  const Id drawing = w.new_drawing("Sheet");
  REQUIRE_FALSE(drawing.is_null());
  QApplication::processEvents();
  // One window: the sheet is a page of the central stack.
  CHECK_FALSE(w.drawing_window().isWindow());
  CHECK(w.drawing_window().window() == &w);
  CHECK(w.drawing_window().isVisible());
  CHECK_FALSE(w.viewport().isVisible());
  CHECK(w.context() == app::Context::Drawing);
  CHECK(w.size() == before);
  // Back to the layout: the viewport again.
  REQUIRE(w.enter_context(app::Context::Layout));
  QApplication::processEvents();
  CHECK_FALSE(w.drawing_window().isVisible());
  CHECK(w.viewport().isVisible());
  // And the tab brings it back.
  REQUIRE(w.enter_context(app::Context::Drawing));
  QApplication::processEvents();
  CHECK(w.drawing_window().isVisible());
  CHECK(w.shown_drawing() == drawing);
}

TEST_CASE("Rows show the evaluation's statuses once it is done, in the window's colours", "[app][gui][browser]") {
  app::MainWindow w;
  w.show();
  CHECK(w.palette().color(QPalette::Highlight) == app::theme::kAccent);
  REQUIRE(w.open_project(samples::source_dir() / "libs" / "vs_proc" / "tests" / "data" / "bolt_bosses.vsproj"));
  settle(w);
  for (QTreeWidgetItemIterator it(&w.volume_tree()); *it != nullptr; ++it) {
    if ((*it)->parent() == nullptr) continue;
    CHECK_FALSE((*it)->text(2).startsWith(QStringLiteral("Regenerating")));
  }
}

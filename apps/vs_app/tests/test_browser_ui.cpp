// The Browser and the folding panels (U1 phase 4, decision 0039): the left
// panels are sections of one Browser, sections fold to a line, the Browser
// and the Inspector fold to a strip and give the viewport the room, and
// sketch mode folds the Browser for as long as it is open.

#include <QApplication>
#include <QDockWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>

#include "browser.hpp"
#include "main_window.hpp"
#include "samples.hpp"
#include "sketch_mode.hpp"
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

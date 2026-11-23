// The context bar in the window (U1 phase 2, decision 0039): one action per
// catalogue tool on every toolbar it belongs to, the menus that are left,
// tabs that follow what is open, and tools that reach the entry points
// their menu items did.

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMenuBar>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

#include "context_bar.hpp"
#include "main_window.hpp"
#include "samples.hpp"
#include "sketch_mode.hpp"
#include "tool_catalog.hpp"
#include "wait.hpp"

using namespace vs;
using app::Context;
using app::Selection;
using app::ToolId;
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

Id volume_named(app::MainWindow& w, const std::string& name) {
  for (const auto& v : w.history().project().document.volumes) {
    if (v.name == name) return v.id;
  }
  return {};
}

Id first_layout_sketch(app::MainWindow& w) {
  for (const auto& f : w.history().project().document.layout.features()) {
    if (f.kind() == feature::FeatureKind::Sketch) return f.id;
  }
  return {};
}

// Sketch mode goes by deleteLater().
void drop_deleted() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }

}  // namespace

TEST_CASE("Every tool is one action, shown on each toolbar it belongs to", "[app][gui][tools]") {
  app::MainWindow w;
  w.show();
  app::ContextBar& bar = w.context_bar();

  int tool_actions = 0;
  for (QAction* a : w.findChildren<QAction*>()) {
    if (a->objectName().startsWith(QStringLiteral("tool_"))) ++tool_actions;
  }
  CHECK(tool_actions == static_cast<int>(app::all_tools().size()));

  for (const app::Tool& t : app::all_tools()) {
    QAction& a = w.tool_action(t.id);
    CHECK(a.objectName() == QStringLiteral("tool_") + QString::fromUtf8(t.name.data(), static_cast<qsizetype>(t.name.size())));
    CHECK_FALSE(a.icon().isNull());
    CHECK(a.shortcut() == QKeySequence(QString::fromUtf8(t.shortcut.data(), static_cast<qsizetype>(t.shortcut.size()))));
  }

  for (Context c : app::kContexts) {
    const auto& groups = app::tool_groups(c);
    for (std::size_t g = 0; g < groups.size(); ++g) {
      const app::ToolGroup& group = groups[g];
      const std::size_t shown = std::min(group.shown, group.tools.size());
      for (std::size_t i = 0; i < group.tools.size(); ++i) {
        QAction* a = &w.tool_action(group.tools[i]);
        if (i < shown) {
          QToolButton* b = bar.button(c, group.tools[i]);
          REQUIRE(b != nullptr);
          CHECK(b->defaultAction() == a);
        } else {
          CHECK(bar.button(c, group.tools[i]) == nullptr);
          QToolButton* more = bar.more_button(c, g);
          REQUIRE(more != nullptr);
          REQUIRE(more->menu() != nullptr);
          CHECK(more->menu()->actions().contains(a));
        }
      }
      CHECK((bar.more_button(c, g) != nullptr) == (shown < group.tools.size()));
    }
  }
}

TEST_CASE("The menus keep File, Edit, View and Help", "[app][gui][tools]") {
  app::MainWindow w;
  QStringList titles;
  for (QAction* a : w.menuBar()->actions()) titles << a->text();
  CHECK(titles == QStringList{QStringLiteral("&File"), QStringLiteral("&Edit"), QStringLiteral("V&iew"),
                              QStringLiteral("&Help")});
  // The shortcuts the removed menus held are on the tools now, or the window's.
  const auto holder = [&](const QKeySequence& key) {
    int n = 0;
    for (QAction* a : w.findChildren<QAction*>()) n += a->shortcut() == key ? 1 : 0;
    return n;
  };
  for (const char* key : {"Ctrl+Shift+K", "Ctrl+Shift+S", "Ctrl+Shift+E", "Ctrl+E", "Ctrl+G", "Ctrl+R", "F5", "Esc"}) {
    INFO(key);
    CHECK(holder(QKeySequence(QString::fromLatin1(key))) == 1);
  }
}

TEST_CASE("The tabs follow what is open, and say why when they can't be entered", "[app][gui][tools]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bracket_path()));
  settle(w);
  app::ContextBar& bar = w.context_bar();
  CHECK(w.context() == Context::Layout);
  CHECK(bar.context() == Context::Layout);
  CHECK(bar.tab(Context::Layout)->isChecked());

  // Nothing selected: Contents and Graph need a volume.
  w.select(Selection::none());
  CHECK_FALSE(bar.tab(Context::Contents)->isEnabled());
  CHECK(bar.tab(Context::Contents)->toolTip().contains(QStringLiteral("Select a volume")));
  CHECK_FALSE(w.enter_context(Context::Contents));
  CHECK(w.context() == Context::Layout);
  CHECK_FALSE(w.tool_action(ToolId::EditContents).isEnabled());
  CHECK(w.tool_action(ToolId::EditContents).toolTip().contains(QStringLiteral("Select a volume first.")));

  // Base is modelled: Contents opens it in context, Done comes back.
  const Id base = volume_named(w, "Base");
  REQUIRE_FALSE(base.is_null());
  w.select(Selection::of_volume(base));
  CHECK(bar.tab(Context::Contents)->isEnabled());
  CHECK(w.tool_action(ToolId::EditContents).isEnabled());
  // A static volume can't take a procedure.
  const Id upright = volume_named(w, "Upright");
  REQUIRE_FALSE(upright.is_null());
  w.select(Selection::of_volume(upright));
  CHECK_FALSE(bar.tab(Context::Graph)->isEnabled());
  CHECK_FALSE(bar.tab(Context::Graph)->toolTip().isEmpty());

  w.select(Selection::of_volume(base));
  REQUIRE(w.enter_context(Context::Contents));
  CHECK(w.editing_volume() == base);
  CHECK(bar.context() == Context::Contents);
  CHECK(bar.tab(Context::Contents)->isChecked());
  CHECK(bar.detail().contains(QStringLiteral("Base")));
  CHECK(w.tool_action(ToolId::Extrude).isEnabled());
  CHECK(w.tool_action(ToolId::LeaveContext).isEnabled());
  CHECK_FALSE(w.tool_action(ToolId::NewVolume).isEnabled());  // a layout tool
  w.tool_action(ToolId::LeaveContext).trigger();
  CHECK(w.editing_volume().is_null());
  CHECK(w.context() == Context::Layout);
  CHECK(bar.tab(Context::Layout)->isChecked());

  // The Layout tab leaves contents edited in context too.
  REQUIRE(w.edit_in_context(base));
  CHECK(w.context() == Context::Contents);
  REQUIRE(w.enter_context(Context::Layout));
  CHECK(w.editing_volume().is_null());
}

TEST_CASE("A sketch selects the Sketch tab, and Finish and Cancel go back", "[app][gui][tools]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bracket_path()));
  settle(w);
  app::ContextBar& bar = w.context_bar();
  const Id sketch = first_layout_sketch(w);
  REQUIRE_FALSE(sketch.is_null());

  w.select(Selection::of_feature(sketch));
  CHECK(bar.tab(Context::Sketch)->isEnabled());
  REQUIRE(w.enter_context(Context::Sketch));
  REQUIRE(w.sketch_mode() != nullptr);
  CHECK(bar.context() == Context::Sketch);
  // Every other tab waits for Finish or Cancel.
  for (Context c : {Context::Layout, Context::Contents, Context::Graph, Context::Drawing}) {
    CHECK_FALSE(bar.tab(c)->isEnabled());
    CHECK(bar.tab(c)->toolTip().contains(QStringLiteral("Finish or cancel")));
  }
  // Constraint tools follow the sketch's selection.
  w.sketch_mode()->view().set_selection({});
  QApplication::processEvents();
  CHECK_FALSE(w.tool_action(ToolId::Horizontal).isEnabled());
  CHECK(w.tool_action(ToolId::SketchLine).isEnabled());
  w.tool_action(ToolId::SketchLine).trigger();
  CHECK(w.sketch_mode()->editor().tool() == app::SketchTool::Line);

  w.tool_action(ToolId::CancelSketch).trigger();
  drop_deleted();
  CHECK(w.sketch_mode() == nullptr);
  CHECK(w.context() == Context::Layout);
  CHECK(bar.context() == Context::Layout);

  w.select(Selection::of_feature(sketch));
  REQUIRE(w.enter_context(Context::Sketch));
  w.tool_action(ToolId::FinishSketch).trigger();
  drop_deleted();
  CHECK(w.sketch_mode() == nullptr);
  CHECK(bar.context() == Context::Layout);
}

TEST_CASE("Tools reach the entry points their menu items did", "[app][gui][tools]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(bracket_path()));
  settle(w);

  // Create: a volume's dialog.
  w.tool_action(ToolId::NewVolume).trigger();
  CHECK(w.open_dialog() != nullptr);
  w.open_dialog()->reject();
  QApplication::processEvents();
  drop_deleted();
  CHECK(w.open_dialog() == nullptr);

  // Layout geometry: a feature's dialog.
  w.tool_action(ToolId::Extrude).trigger();
  CHECK(w.open_dialog() != nullptr);
  w.open_dialog()->reject();
  QApplication::processEvents();
  drop_deleted();

  // Drawing: New opens the Drawing tab, Close leaves it.
  CHECK_FALSE(w.tool_action(ToolId::NewDrawing).isEnabled());  // a Drawing tool
  REQUIRE(w.enter_context(Context::Drawing));
  CHECK(w.context() == Context::Drawing);
  CHECK_FALSE(w.shown_drawing().is_null());
  CHECK(w.tool_action(ToolId::AddDrawingView).isEnabled());
  w.tool_action(ToolId::CloseDrawing).trigger();
  QApplication::processEvents();
  CHECK(w.shown_drawing().is_null());
  CHECK(w.context() == Context::Layout);
  CHECK(w.context_bar().context() == Context::Layout);
}

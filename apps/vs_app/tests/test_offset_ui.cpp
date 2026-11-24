// Offset in the window (decision 0041): the Sketch tab's tool waits for a
// curve, the dialog previews on the sketch, OK keeps one sketch step,
// Cancel takes it out, and the inset nests in its source as a hole.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>

#include <catch2/catch_test_macros.hpp>

#include "main_window.hpp"
#include "offset_dialog.hpp"
#include "samples.hpp"
#include "sketch_mode.hpp"
#include "tool_catalog.hpp"
#include "vs/sketch/profile.hpp"
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

std::size_t count(const sketch::Sketch& s, sketch::EntityKind kind) {
  std::size_t n = 0;
  for (const auto& e : s.entities()) n += e.kind() == kind ? 1 : 0;
  return n;
}

}  // namespace

TEST_CASE("Offset previews on the sketch and keeps one step", "[app][gui][offset]") {
  app::MainWindow w;
  w.show();
  w.new_project("Offset");
  settle(w);
  app::SketchMode* mode = w.new_sketch(feature::PrincipalPlane::XY, "Plate");
  REQUIRE(mode != nullptr);
  CHECK(app::shows_in(app::ToolId::OffsetCurves, app::Context::Sketch));

  // A 40 × 20 rectangle.
  mode->set_tool(app::SketchTool::Rectangle);
  auto& ed = mode->editor();
  ed.click({0, 0});
  REQUIRE(!ed.click({40, 20}).empty());
  mode->set_tool(app::SketchTool::Select);
  mode->refresh();
  std::vector<Id> lines;
  for (const auto& e : ed.sketch().entities())
    if (e.kind() == sketch::EntityKind::Line) lines.push_back(e.id);
  REQUIRE(lines.size() == 4);

  // Nothing selected: the tool waits and the dialog doesn't open.
  mode->view().set_selection({});
  CHECK_FALSE(w.tool_action(app::ToolId::OffsetCurves).isEnabled());
  CHECK(w.offset_curves() == nullptr);

  // One side selected: the whole rectangle offsets.
  mode->view().set_selection({lines[0]});
  CHECK(w.tool_action(app::ToolId::OffsetCurves).isEnabled());
  const sketch::Sketch before = ed.sketch();

  SECTION("Cancel takes the preview out") {
    app::OffsetDialog* d = w.offset_curves();
    REQUIRE(d != nullptr);
    CHECK(mode->offset_previewing());
    CHECK(count(ed.sketch(), sketch::EntityKind::Line) == 8);
    d->reject();
    CHECK_FALSE(mode->offset_previewing());
    CHECK(ed.sketch() == before);
    mode->finish(false);
  }

  SECTION("options preview, OK keeps one step, the inset is a hole") {
    app::OffsetDialog* d = w.offset_curves();
    REQUIRE(d != nullptr);
    d->corners().setCurrentIndex(1);  // round: four corner arcs outside
    CHECK(count(ed.sketch(), sketch::EntityKind::Arc) == 4);
    d->side().setCurrentIndex(1);  // inward: trimmed, no arcs
    d->distance().setValue(3);
    CHECK(count(ed.sketch(), sketch::EntityKind::Arc) == 0);
    CHECK(count(ed.sketch(), sketch::EntityKind::Line) == 8);
    // Too deep: refused with a reason, nothing previewed.
    d->distance().setValue(15);
    CHECK_FALSE(mode->offset_previewing());
    CHECK(ed.sketch() == before);
    CHECK_FALSE(d->message().isEmpty());
    // Only the selected side, not its chain: one line.
    d->distance().setValue(3);
    d->chains().setChecked(false);
    CHECK(count(ed.sketch(), sketch::EntityKind::Line) == 5);
    d->chains().setChecked(true);
    d->commit();
    CHECK(w.offset_dialog() == nullptr);
    CHECK_FALSE(mode->offset_previewing());
    CHECK(count(ed.sketch(), sketch::EntityKind::Line) == 8);
    auto profiles = sketch::extract_profiles(ed.sketch(), w.session().settings());
    REQUIRE(profiles.ok());
    REQUIRE(profiles->size() == 1);
    CHECK((*profiles)[0].holes.size() == 1);
    // One step back is the rectangle.
    REQUIRE(ed.undo());
    CHECK(ed.sketch() == before);
    ed.redo();
    mode->refresh();
    mode->finish(true);
    settle(w);
    CHECK(w.sketch_mode() == nullptr);
  }
}

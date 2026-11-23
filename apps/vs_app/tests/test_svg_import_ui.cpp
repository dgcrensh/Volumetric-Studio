// Import SVG and the Curve tool in the window (S1, decision 0040): the
// dialog previews on the sketch, OK keeps one sketch step, Cancel takes it
// out, and an imported outline extrudes into a solid.

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "authoring.hpp"
#include "main_window.hpp"
#include "samples.hpp"
#include "sketch_mode.hpp"
#include "svg_import_dialog.hpp"
#include "tool_catalog.hpp"
#include "vs/feature/rebuild.hpp"
#include "vs/kernel/queries.hpp"
#include "wait.hpp"

using namespace vs;
using core::Id;
using Catch::Matchers::WithinRel;

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
  for (const auto& e : s.entities()) n += e.kind() == kind && !e.construction ? 1 : 0;
  return n;
}

// A 40 mm drop outline of two curves, a 4 mm hole and an open stroke.
constexpr const char* kSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="40mm" height="40mm"
     viewBox="0 0 40 40">
  <path d="M5 20 C5 5 35 5 35 20 S5 35 5 20 Z" style="fill:#f00"/>
  <circle cx="20" cy="20" r="4"/>
  <path d="M0 38 L40 38" stroke="black"/>
</svg>)svg";

}  // namespace

TEST_CASE("Import SVG previews on the sketch, keeps one step and extrudes", "[app][gui][svg]") {
  QTemporaryDir dir;
  REQUIRE(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("drop.svg"));
  {
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(kSvg);
  }
  app::MainWindow w;
  w.show();
  w.new_project("Svg");
  settle(w);
  app::SketchMode* mode = w.new_sketch(feature::PrincipalPlane::XY, "Drop");
  REQUIRE(mode != nullptr);
  // The Insert group's SVG tool is on the Sketch tab and usable.
  CHECK(app::shows_in(app::ToolId::ImportSvg, app::Context::Sketch));

  SECTION("Cancel takes the preview out") {
    app::SvgImportDialog* d = w.import_svg(path);
    REQUIRE(d != nullptr);
    CHECK(mode->import_previewing());
    CHECK(count(mode->editor().sketch(), sketch::EntityKind::Bezier) == 2);
    CHECK(d->message().contains(QStringLiteral("construction")));
    d->reject();
    CHECK_FALSE(mode->import_previewing());
    CHECK(mode->editor().sketch().entities().empty());
    CHECK_FALSE(mode->editor().can_undo());
    mode->finish(false);
  }

  SECTION("options preview, OK keeps one step, Finish extrudes it") {
    app::SvgImportDialog* d = w.import_svg(path);
    REQUIRE(d != nullptr);
    d->fix().setChecked(true);
    CHECK(mode->editor().status() == sketch::SolveStatus::Solved);
    d->fix().setChecked(false);
    d->scale().setValue(2);
    // Each preview replaced the last: still one drop, one circle.
    CHECK(count(mode->editor().sketch(), sketch::EntityKind::Bezier) == 2);
    CHECK(count(mode->editor().sketch(), sketch::EntityKind::Circle) == 1);
    d->commit();
    CHECK(w.svg_import_dialog() == nullptr);
    CHECK_FALSE(mode->import_previewing());
    CHECK(mode->editor().can_undo());
    mode->editor().undo();
    CHECK_FALSE(mode->editor().can_undo());
    mode->editor().redo();
    mode->refresh();

    // The Curve tool draws on the same sketch: a separate closed drop.
    mode->set_tool(app::SketchTool::Bezier);
    auto& ed = mode->editor();
    ed.click({100, 0});
    ed.click({100, 30});
    ed.click({140, 30});
    REQUIRE(ed.click({140, 0}).size() == 5);
    const Id start = ed.nearest_point({100, 0}, 0.01);
    ed.click({140, -30});
    ed.click({100, -30});
    REQUIRE(ed.click({100, 0}, start).size() == 3);
    mode->refresh();

    mode->finish(true);
    settle(w);
    REQUIRE(w.sketch_mode() == nullptr);
    REQUIRE(w.history().undo_count() == 1);
    const auto& layout = w.history().project().document.layout;
    REQUIRE(layout.features().size() == 1);
    const Id sketch_id = layout.features()[0].id;

    feature::ExtrudeFeature e;
    e.sketch = sketch_id;
    e.depth = feature::Param::of(5);
    feature::Feature extrude;
    extrude.definition = e;
    REQUIRE_FALSE(w.apply(app::add_feature({}, extrude)).has_errors());
    settle(w);
    const auto built = feature::rebuild(w.history().project().document.layout, w.session().settings());
    REQUIRE(built.ok());
    const auto m = kernel::mass_properties(built.body);
    REQUIRE(m.ok());
    CHECK(m->volume > 0);
    CHECK(built.body.solid_count() == 2);
  }
}

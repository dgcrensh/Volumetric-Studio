// The main window on a real display and GPU: panels, selection both ways,
// export from the menus' code path, the rendered viewport, and the UI
// staying responsive while a long procedural volume regenerates.

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QScreen>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "main_window.hpp"
#include "samples.hpp"
#include "viewport.hpp"
#include "vs/mesh/io.hpp"
#include "wait.hpp"

using namespace vs;
using app::Selection;

namespace {

bool wait_shown(app::MainWindow& w, int timeout_ms = 120000) {
  bool shown = false;
  auto c = QObject::connect(&w, &app::MainWindow::evaluationShown, [&] { shown = true; });
  const bool ok = samples::wait_until([&] { return shown; }, timeout_ms);
  QObject::disconnect(c);
  return ok;
}

QTreeWidgetItem* volume_item(QTreeWidget& tree, const core::Id& id) {
  const QString key = QString::fromStdString(id.to_string());
  // Children of composites sit under their parent's row (L3).
  for (QTreeWidgetItemIterator it(&tree); *it != nullptr; ++it) {
    if ((*it)->parent() != nullptr && (*it)->data(0, Qt::UserRole + 1).toString() == key) return *it;
  }
  return nullptr;
}

// How much of the image differs from its top-left pixel (the background
// gradient's first row is uniform).
double coverage(const QImage& image) {
  if (image.isNull()) return 0.0;
  std::size_t differ = 0;
  const QColor bg = image.pixelColor(image.width() / 2, 2);
  for (int y = 0; y < image.height(); y += 4) {
    const QColor rowbg = image.pixelColor(2, y);
    for (int x = 0; x < image.width(); x += 4) {
      const QColor c = image.pixelColor(x, y);
      const int d = std::abs(c.red() - rowbg.red()) + std::abs(c.green() - rowbg.green()) +
                    std::abs(c.blue() - rowbg.blue());
      if (d > 30) ++differ;
    }
  }
  (void)bg;
  return static_cast<double>(differ) / (static_cast<double>(image.width() / 4) * (image.height() / 4));
}

}  // namespace

TEST_CASE("The window opens each sample project and draws it", "[app][gui][acceptance]") {
  app::MainWindow w;
  w.resize(1280, 800);
  w.show();
  REQUIRE(w.viewport().has_gpu());
  for (const auto& path : samples::projects()) {
    const std::string name = path.filename().string();
    INFO(name);
    REQUIRE(w.open_project(path));
    REQUIRE(wait_shown(w));
    const auto s = w.session().latest();
    REQUIRE(s);
    // Secondary items (sketches features use) show only while selected.
    const auto shown = static_cast<std::size_t>(std::count_if(
        s->scene.items.begin(), s->scene.items.end(), [](const auto& item) { return !item.secondary; }));
    CHECK(w.viewport().displayed_items() == shown);
    // Every volume has a row; every diagnostic has a row.
    CHECK(w.volume_tree().topLevelItemCount() >= (s->evaluation.document.volumes.empty() ? 0 : 1));
    for (const auto& v : s->evaluation.document.volumes) CHECK(volume_item(w.volume_tree(), v.id) != nullptr);
    core::Diagnostics all = s->evaluation.diagnostics;
    all.append(s->scene.diagnostics);
    CHECK(w.diagnostics_list().topLevelItemCount() == static_cast<int>(all.size()));
    if (!s->scene.items.empty()) {
      QApplication::processEvents();
      const QImage image = w.viewport().render_image();
      REQUIRE(!image.isNull());
      // Something is drawn, and the view is framed on it.
      const double c = coverage(image);
      CHECK(c > 0.03);
      CHECK(c < 0.98);
    }
  }
}

TEST_CASE("Each kind of scene item is visible on its own", "[app][gui]") {
  app::Viewport viewport;
  viewport.resize(800, 600);
  viewport.show();
  REQUIRE(viewport.has_gpu());
  const auto ev = samples::evaluate(samples::load("bolt_bosses.vsproj"));
  REQUIRE(ev.ok());
  const auto scene = app::build_scene(ev, core::ProductSettings::defaults());
  for (const auto role : {app::ItemRole::Boundary, app::ItemRole::Contents, app::ItemRole::ConnectionSurface}) {
    INFO(app::to_string(role));
    app::Scene only;
    for (const auto& item : scene.items) {
      if (item.role == role) only.items.push_back(item);
    }
    only.bounds = scene.bounds;
    REQUIRE(!only.items.empty());
    viewport.show_scene(only, nullptr, true);
    QApplication::processEvents();
    const QImage image = viewport.render_image();
    CHECK(coverage(image) > 0.01);
    // What reaches the screen is the new scene, not the last one: compare
    // the window's pixels with the off-screen render.
    samples::wait_until([] { return false; }, 300);
    const QImage screen = viewport.screen()->grabWindow(viewport.winId()).toImage();
    const bool same = std::abs(coverage(screen) - coverage(image)) < 0.05;
    CHECK(same);
    if (!same) {
      // Keep both for a look.
      const auto dir = std::filesystem::temp_directory_path();
      const std::string name(app::to_string(role));
      screen.save(QString::fromStdWString((dir / ("vs_app_screen_" + name + ".bmp")).wstring()));
      image.save(QString::fromStdWString((dir / ("vs_app_render_" + name + ".bmp")).wstring()));
    }
  }
}

TEST_CASE("Selecting in the tree, the viewport and the diagnostics list agree", "[app][gui][acceptance]") {
  app::MainWindow w;
  w.show();
  const auto overlap = samples::source_dir() / "libs" / "vs_validate" / "tests" / "data" / "overlap.vsproj";
  REQUIRE(w.open_project(overlap));
  REQUIRE(wait_shown(w));
  const auto s = w.session().latest();
  const auto& doc = s->evaluation.document;
  REQUIRE(doc.volumes.size() >= 2);

  // Tree -> selection -> inspector and feature panel.
  auto* item = volume_item(w.volume_tree(), doc.volumes[1].id);
  REQUIRE(item != nullptr);
  w.volume_tree().setCurrentItem(item);
  CHECK(w.selection() == Selection::of_volume(doc.volumes[1].id));
  CHECK(w.inspector().topLevelItemCount() > 0);

  // Viewport pick -> tree.
  emit w.viewport().picked(doc.volumes[0].id);
  CHECK(w.selection() == Selection::of_volume(doc.volumes[0].id));
  auto* first = volume_item(w.volume_tree(), doc.volumes[0].id);
  REQUIRE(first != nullptr);
  CHECK(first->isSelected());
  CHECK_FALSE(item->isSelected());

  // Empty space clears it.
  emit w.viewport().picked(core::Id{});
  CHECK(w.selection().empty());

  // Diagnostics list -> the act-here target.
  int row = -1;
  for (int i = 0; i < w.diagnostics_list().topLevelItemCount(); ++i) {
    if (w.diagnostics_list().topLevelItem(i)->text(1) == QLatin1String("validate.overlap")) row = i;
  }
  REQUIRE(row >= 0);
  w.activate_diagnostic(row);
  CHECK(w.selection() == Selection::of_volume(doc.volumes[0].id));
  CHECK(volume_item(w.volume_tree(), doc.volumes[0].id)->isSelected());
}

TEST_CASE("Export from the app writes STL and 3MF that read back", "[app][gui][acceptance]") {
  app::MainWindow w;
  w.show();
  const auto bracket = samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj";
  REQUIRE(w.open_project(bracket));
  REQUIRE(wait_shown(w));
  for (const auto format : {app::ExportFormat::Stl, app::ExportFormat::ThreeMf}) {
    const auto path = std::filesystem::temp_directory_path() /
                      (std::string("vs_app_window_export.") + std::string(app::extension(format)));
    std::filesystem::remove(path);
    bool done = false;
    app::ExportResult result;
    w.export_to(path, format, [&](const app::ExportResult& r) {
      result = r;
      done = true;
    });
    REQUIRE(samples::wait_until([&] { return done; }, 60000));
    INFO(samples::codes(result.diagnostics));
    REQUIRE(result.ok());
    REQUIRE(std::filesystem::exists(path));
    std::ifstream f(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::transform(chars.begin(), chars.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    const auto back = format == app::ExportFormat::Stl ? mesh::read_stl(bytes) : mesh::read_3mf(bytes);
    REQUIRE(back.ok());
    CHECK(back->triangle_count() == result.triangles);
    f.close();
    std::filesystem::remove(path);
  }
}

TEST_CASE("The UI stays responsive while a long procedural volume regenerates", "[app][gui][acceptance]") {
  app::MainWindow w;
  w.resize(1280, 800);
  w.show();
  REQUIRE(w.viewport().has_gpu());
  // Warm up: the first frames of a fresh viewer set up OpenGL and compile
  // shaders (one-time costs of starting the app, not of regenerating), so
  // show a small project first.
  {
    bool warm = false;
    auto c = QObject::connect(&w, &app::MainWindow::evaluationShown, [&] { warm = true; });
    w.session().set_project(samples::load("bolt_bosses.vsproj"));
    REQUIRE(samples::wait_until([&] { return warm; }, 60000));
    QObject::disconnect(c);
    samples::wait_until([] { return false; }, 500);
  }

  // Measure the gaps between UI-thread timer ticks (each tick also asks the
  // viewport for a frame) and every frame's render time.
  double max_gap = 0, max_frame = 0;
  std::size_t ticks = 0, frames = 0;
  QElapsedTimer clock;
  QTimer ticker;
  ticker.setTimerType(Qt::PreciseTimer);
  ticker.setInterval(10);
  qint64 last = -1;
  QObject::connect(&ticker, &QTimer::timeout, [&] {
    const qint64 now = clock.nsecsElapsed();
    if (last >= 0) max_gap = std::max(max_gap, static_cast<double>(now - last) / 1e6);
    last = now;
    ++ticks;
    w.viewport().update();
  });
  std::size_t slow_frames = 0;  // over 50 ms
  QObject::connect(&w.viewport(), &app::Viewport::frameRendered, [&](double ms) {
    max_frame = std::max(max_frame, ms);
    if (ms > 50.0) ++slow_frames;
    ++frames;
  });

  bool shown = false;
  QObject::connect(&w, &app::MainWindow::evaluationShown, [&] { shown = true; });
  clock.start();
  ticker.start();
  w.session().set_project(samples::long_procedural(0.15));
  REQUIRE(samples::wait_until([&] { return shown; }, 300000));
  // A few more frames with the big mesh on screen, orbiting.
  const qint64 regenerated_ns = clock.nsecsElapsed();
  samples::wait_until([] { return false; }, 1000);
  ticker.stop();

  const auto s = w.session().latest();
  REQUIRE(s);
  INFO(samples::codes(s->evaluation.diagnostics));
  CHECK(s->evaluation.ok());
  const auto* contents = s->scene.find(s->evaluation.document.volumes.at(0).id, app::ItemRole::Contents);
  REQUIRE(contents != nullptr);
  UNSCOPED_INFO("regeneration " << s->evaluate_ms << " ms, display data " << s->scene_ms << " ms, "
                                << contents->mesh.triangle_count() << " triangles, " << ticks << " ticks, "
                                << frames << " frames, longest gap " << max_gap << " ms, longest frame "
                                << max_frame << " ms");
  WARN("regeneration " << s->evaluate_ms << " ms, display data " << s->scene_ms << " ms, "
                       << contents->mesh.triangle_count() << " triangles; longest UI gap " << max_gap
                       << " ms, longest frame " << max_frame << " ms over " << frames << " frames ("
                       << slow_frames << " over 50 ms)");
  // The regeneration was long enough to mean something.
  CHECK(static_cast<double>(regenerated_ns) / 1e6 > 1000.0);
  CHECK(frames > 50);
  CHECK(max_gap < 100.0);
  CHECK(max_frame < 100.0);
}

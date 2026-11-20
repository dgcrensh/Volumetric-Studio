// M17: Validate before export, STEP export, import as a new volume and
// project caches, from the app.

#include <QTreeWidget>

#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "main_window.hpp"
#include "samples.hpp"
#include "ship.hpp"
#include "vs/eval/step_export.hpp"
#include "vs/kernel/exchange.hpp"
#include "vs/mesh/io.hpp"

using namespace vs;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

struct TempDir {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("vs_app_ship_" + core::Id::random().to_string());
  TempDir() { std::filesystem::create_directories(path); }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

bool wait_shown(app::MainWindow& w, int timeout_ms = 120000) {
  bool shown = false;
  auto c = QObject::connect(&w, &app::MainWindow::evaluationShown, [&] { shown = true; });
  const bool ok = samples::wait_until([&] { return shown; }, timeout_ms);
  QObject::disconnect(c);
  return ok;
}

// A closed box mesh, wound outward.
mesh::Mesh box_mesh(core::fvec3 lo, core::fvec3 hi) {
  mesh::Mesh m;
  for (int i = 0; i < 8; ++i) {
    m.add_vertex({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
  }
  const core::Id v = core::Id::random();
  const std::uint32_t quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                     {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (const auto& q : quads) {
    m.add_triangle(q[0], q[1], q[2], v);
    m.add_triangle(q[0], q[2], q[3], v);
  }
  return m;
}

}  // namespace

TEST_CASE("Validate sums up every check before export", "[app][ship]") {
  SECTION("a clean design is ready") {
    const auto project = samples::load("bracket.vsproj");
    const auto ev = samples::evaluate(project);
    const auto check = app::check_for_export(project, ev, kSettings);
    INFO(samples::codes(check.diagnostics));
    CHECK(check.ok());
    CHECK(check.volumes == 2);
    CHECK(check.current == 2);
    CHECK(app::summary(check).starts_with("Ready to export"));
  }
  SECTION("overlap stops it, once, with the reason") {
    const auto project = samples::load("overlap.vsproj");
    const auto ev = samples::evaluate(project);
    const auto check = app::check_for_export(project, ev, kSettings);
    CHECK_FALSE(check.ok());
    // Both validate() and the engine see the overlap: it is listed once.
    CHECK(check.diagnostics.with_code("validate.overlap").size() == 1);
    CHECK(app::summary(check).starts_with("Not ready to export"));
  }
  SECTION("generated contents that stick out are caught") {
    const auto project = samples::load("containment.vsproj");
    const auto ev = samples::evaluate(project);
    const auto check = app::check_for_export(project, ev, kSettings);
    CHECK_FALSE(check.diagnostics.with_code("validate.containment").empty());
    CHECK_FALSE(check.ok());
  }
  SECTION("a result that isn't current is a warning") {
    const auto project = samples::load("shell_and_ball.vsproj");
    eval::Engine engine(kSettings, {.threads = 2});
    const auto preview = engine.preview(project);  // no generator: nothing generated
    const auto check = app::check_for_export(project, preview, kSettings);
    CHECK_FALSE(check.diagnostics.with_code("app.validate.stale").empty());
    CHECK(check.warnings >= 1);
  }
}

TEST_CASE("The app validates, imports, exports STEP and saves caches", "[app][gui][ship][acceptance]") {
  TempDir dir;
  app::MainWindow w;
  w.resize(1280, 800);
  w.show();
  REQUIRE(w.open_project(samples::source_dir() / "libs/vs_model/tests/data/bracket.vsproj"));
  REQUIRE(wait_shown(w));

  // Validate.
  std::optional<app::ShipCheck> check;
  w.validate([&](const app::ShipCheck& c) { check = c; });
  REQUIRE(samples::wait_until([&] { return check.has_value(); }, 60000));
  INFO(samples::codes(check->diagnostics));
  CHECK(check->ok());
  REQUIRE(w.last_check());

  // Import an STL beside the bracket as a new volume.
  const auto stl = mesh::write_stl(box_mesh({100, 0, 0}, {110, 10, 10}));
  REQUIRE(stl.ok());
  const auto stl_path = dir.path / "puck.stl";
  {
    std::ofstream f(stl_path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(stl->data()), static_cast<std::streamsize>(stl->size()));
  }
  const std::size_t before = w.history().project().document.volumes.size();
  REQUIRE_FALSE(w.import_file(stl_path).has_errors());
  REQUIRE(w.history().project().document.volumes.size() == before + 1);
  CHECK(w.history().project().document.volumes.back().name == "puck");
  REQUIRE(wait_shown(w));
  CHECK(w.import_file(dir.path / "missing.stl").has_errors());

  // STEP: the bracket's two B-rep parts and the puck as a mesh body.
  std::optional<eval::StepExport> step;
  const auto step_path = dir.path / "bracket.step";
  w.export_step_to(step_path, {}, [&](const eval::StepExport& r) { step = r; });
  REQUIRE(samples::wait_until([&] { return step.has_value(); }, 60000));
  INFO(samples::codes(step->diagnostics));
  REQUIRE(step->ok());
  CHECK(step->breps == 2);
  CHECK(step->meshes == 1);
  const auto parts = kernel::read_step_parts(step_path);
  REQUIRE(parts.ok());
  CHECK(parts->size() == 3);

  // Save with caches, reopen: the static volumes come from the cache.
  w.set_save_caches(true);
  const auto saved = dir.path / "bracket.vsproj";
  REQUIRE_FALSE(w.save_to(saved).has_errors());
  CHECK(model::load_project_cache(saved).size() == 4);  // Upright and puck: .json and contents each

  app::MainWindow again;
  again.resize(1280, 800);
  again.show();
  REQUIRE(again.open_project(saved));
  REQUIRE(wait_shown(again));
  CHECK(again.session().cache_entries_loaded() == 2);
  const auto& ev = again.session().latest()->evaluation;
  for (const auto& v : ev.volumes) {
    const auto* vol = ev.document.find_volume(v.volume);
    REQUIRE(vol != nullptr);
    INFO(vol->name);
    CHECK(v.status == model::VolumeStatus::Current);
    CHECK(v.from_cache == (vol->kind() == model::VolumeKind::Static));
  }

  // With the option off, saving drops them.
  again.set_save_caches(false);
  REQUIRE_FALSE(again.save_to(saved).has_errors());
  CHECK(model::load_project_cache(saved).empty());
}

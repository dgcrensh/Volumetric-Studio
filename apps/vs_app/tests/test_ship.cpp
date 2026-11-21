// M17: Validate before export (the Qt-free half; the app's own walk is
// test_ship_ui.cpp).

#include <catch2/catch_test_macros.hpp>

#include <variant>

#include "exporter.hpp"
#include "samples.hpp"
#include "ship.hpp"
#include "vs/mesh/io.hpp"

using namespace vs;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

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

TEST_CASE("An unweldable mesh stops STL and 3MF export but not STEP; the weld is made once (H5)",
          "[app][ship]") {
  // The imported hex prism with one triangle gone: it builds and is
  // contained, but its mesh is open, so the weld fails.
  auto project = samples::static_mesh();
  auto& volume = project.document.volumes.at(0);
  auto& stat = std::get<model::StaticContents>(volume.contents);
  auto mesh = mesh::read_mesh(*project.blobs.at(stat.blob), "stl");
  REQUIRE(mesh);
  mesh->indices.resize(mesh->indices.size() - 3);
  mesh->triangle_volumes.pop_back();
  auto bytes = mesh::write_stl(*mesh);
  REQUIRE(bytes);
  project.blobs.erase(stat.blob);
  stat.blob = project.add_blob(std::move(bytes).value(), "stl");

  const auto ev = samples::evaluate(project);
  const auto check = app::check_for_export(project, ev, kSettings);
  INFO(samples::codes(check.diagnostics));
  CHECK_FALSE(check.diagnostics.with_code("mesh.weld.open").empty());
  REQUIRE(check.weld_errors > 0);
  CHECK(check.errors == check.weld_errors);
  CHECK_FALSE(check.ok());
  CHECK_FALSE(check.ok_for(app::ShipTarget::Mesh));
  CHECK(check.ok_for(app::ShipTarget::Step));
  CHECK(app::summary(check, app::ShipTarget::Mesh).starts_with("Not ready to export"));
  CHECK(app::summary(check, app::ShipTarget::Step).starts_with("Ready to export STEP"));

  // The check keeps its weld, and exporting from it refuses as welding
  // again would, with the same errors.
  REQUIRE(check.combined);
  const auto reused = app::export_mesh(ev, *check.combined, app::ExportFormat::Stl);
  const auto fresh = app::export_mesh(ev, project.blobs, kSettings, app::ExportFormat::Stl);
  CHECK_FALSE(reused.ok());
  CHECK(reused.diagnostics.with_code("mesh.weld.open").size() == fresh.diagnostics.with_code("mesh.weld.open").size());

  // Anything else that stops export still stops STEP.
  const auto overlap = samples::load("overlap.vsproj");
  const auto ov = app::check_for_export(overlap, samples::evaluate(overlap), kSettings);
  CHECK_FALSE(ov.ok_for(app::ShipTarget::Step));
  CHECK(app::summary(ov, app::ShipTarget::Step).starts_with("Not ready to export"));
}

TEST_CASE("Exporting from the check's weld writes the same file as welding again (H5)", "[app][ship]") {
  const auto project = samples::load("shell_and_ball.vsproj");
  const auto ev = samples::evaluate(project);
  const auto check = app::check_for_export(project, ev, kSettings);
  REQUIRE(check.ok());
  REQUIRE(check.combined);
  for (const auto format : {app::ExportFormat::Stl, app::ExportFormat::ThreeMf}) {
    const auto reused = app::export_mesh(ev, *check.combined, format);
    const auto fresh = app::export_mesh(ev, project.blobs, kSettings, format);
    REQUIRE(reused.ok());
    CHECK(reused.bytes == fresh.bytes);
    CHECK(reused.triangles == fresh.triangles);
  }
}

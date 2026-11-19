// File > Export: STL and 3MF from what the app shows.

#include <filesystem>
#include <fstream>
#include <iterator>

#include <catch2/catch_test_macros.hpp>

#include "exporter.hpp"
#include "samples.hpp"
#include "vs/mesh/io.hpp"
#include "vs/mesh/topology.hpp"

using namespace vs;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

}  // namespace

TEST_CASE("Every sample project that evaluates cleanly exports STL and 3MF", "[app][export][acceptance]") {
  std::size_t exported = 0;
  for (const auto& path : samples::projects()) {
    const std::string name = path.filename().string();
    INFO(name);
    auto project = model::load_project(path, false);
    REQUIRE(project.ok());
    const auto ev = samples::evaluate(*project);
    const auto stl = app::export_mesh(ev, project->blobs, kSettings, app::ExportFormat::Stl);
    const auto three_mf = app::export_mesh(ev, project->blobs, kSettings, app::ExportFormat::ThreeMf);
    INFO(samples::codes(ev.diagnostics) + samples::codes(stl.diagnostics));
    if (ev.ok() && !stl.ok() && !stl.diagnostics.with_code("mesh.combine.empty").empty()) {
      // Only placeholders (contact.vsproj): nothing to print, and it says so.
      CHECK_FALSE(three_mf.ok());
      continue;
    }
    if (!ev.ok()) {
      // A broken design says why it won't export, or exports what it can.
      CHECK((stl.ok() || stl.diagnostics.has_errors()));
      continue;
    }
    REQUIRE(stl.ok());
    REQUIRE(three_mf.ok());
    ++exported;

    const auto back = mesh::read_stl(stl.bytes);
    REQUIRE(back.ok());
    CHECK(back->triangle_count() == stl.triangles);
    // One welded, closed mesh.
    const auto topology = mesh::analyse(*back);
    CHECK(topology.watertight());

    const auto back3 = mesh::read_3mf(three_mf.bytes);
    REQUIRE(back3.ok());
    CHECK(back3->triangle_count() == three_mf.triangles);
    CHECK(three_mf.objects >= 1);
  }
  // The bracket, bolt bosses, shell and ball, and the clean fixture; the
  // other fixtures are placeholders or deliberately broken.
  CHECK(exported >= 4);
  // And an imported STL (M8's import path) exports as it is.
  const auto imported = samples::static_mesh();
  const auto ev = samples::evaluate(imported);
  const auto r = app::export_mesh(ev, imported.blobs, kSettings, app::ExportFormat::Stl);
  INFO(samples::codes(r.diagnostics));
  REQUIRE(r.ok());
  CHECK(r.triangles == 20);
}

TEST_CASE("Export writes the file", "[app][export]") {
  const auto project = samples::load("shell_and_ball.vsproj");
  const auto ev = samples::evaluate(project);
  REQUIRE(ev.ok());
  const auto r = app::export_mesh(ev, project.blobs, kSettings, app::ExportFormat::ThreeMf);
  REQUIRE(r.ok());
  const auto path = std::filesystem::temp_directory_path() / "vs_app_export_test.3mf";
  CHECK(app::write_file(path, r.bytes).empty());
  CHECK(std::filesystem::file_size(path) == r.bytes.size());
  std::filesystem::remove(path);

  const auto bad = app::write_file(std::filesystem::path("Z:/no/such/folder/out.stl"), r.bytes);
  REQUIRE(bad.has_errors());
  CHECK(bad[0].code == "app.export.io");
}

TEST_CASE("A project that doesn't build refuses to export", "[app][export]") {
  const auto project = samples::load("loop.vsproj");
  const auto ev = samples::evaluate(project);
  const auto r = app::export_mesh(ev, project.blobs, kSettings, app::ExportFormat::Stl);
  CHECK_FALSE(r.ok());
  CHECK(r.diagnostics.has_errors());
  CHECK(r.bytes.empty());
}

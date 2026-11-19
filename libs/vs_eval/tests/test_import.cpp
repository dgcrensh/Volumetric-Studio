#include <filesystem>
#include <fstream>
#include <iterator>

#include <catch2/catch_test_macros.hpp>

#include "bracket.hpp"
#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/eval/import.hpp"

using namespace vs;
using model::VolumeStatus;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

model::Blob read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  model::Blob out(text.size());
  std::transform(text.begin(), text.end(), out.begin(),
                 [](char c) { return static_cast<std::byte>(c); });
  return out;
}

const eval::VolumeResult& result(const eval::Evaluation& ev, const core::Id& id) {
  const auto* r = ev.find(id);
  REQUIRE(r != nullptr);
  return *r;
}

}  // namespace

TEST_CASE("File formats are recognised by extension", "[eval][import]") {
  CHECK(eval::import_format(".STP") == "step");
  CHECK(eval::import_format("step") == "step");
  CHECK(eval::import_format("igs") == "iges");
  CHECK(eval::import_format(".3MF") == "3mf");
  CHECK(eval::import_format("obj") == "obj");
  CHECK(eval::import_format(".dwg").empty());
}

TEST_CASE("A STEP file imports as a static volume and evaluates", "[eval][import]") {
  model::Project project;
  project.document = model::make_document("Imports");
  const auto file = bracket::step_box(5, 5, 5, 35, 25, 15);
  const auto id = eval::import_volume(project, file, "step", "Plate", kSettings);
  REQUIRE(id.ok());
  const model::Volume* v = project.document.find_volume(*id);
  REQUIRE(v != nullptr);
  CHECK(v->kind() == model::VolumeKind::Static);
  CHECK(std::get<model::StaticContents>(v->contents).format == "step");

  eval::Engine engine(kSettings);
  const auto ev = engine.evaluate(project);
  INFO(bracket::codes(ev.diagnostics));
  CHECK(ev.ok());
  const auto& r = result(ev, *id);
  CHECK(r.status == VolumeStatus::Current);
  REQUIRE(r.contents);
  CHECK_FALSE(r.contents->solid.is_empty());

  // Static volumes never regenerate.
  const auto again = engine.evaluate(project);
  CHECK(again.stats.regenerated == 0);
}

TEST_CASE("A mesh file imports as a static volume with measured outputs", "[eval][import]") {
  model::Project project;
  project.document = model::make_document("Imports");
  const auto file = read_file(std::filesystem::path(VS_MESH_TEST_DATA) / "hex_prism.stl");
  const auto id = eval::import_volume(project, file, ".stl", "Hex", kSettings);
  REQUIRE(id.ok());
  model::Volume* v = project.document.find_volume(*id);
  const core::Id out = core::Id::random();
  v->outputs.push_back({out, "volume", model::OutputQuantity::Volume, 1.0});
  const core::Id env = core::Id::random();
  v->outputs.push_back({env, "envelope", model::OutputQuantity::Envelope, 1.0});

  std::vector<core::Hash256> hashes;
  for (const std::size_t threads : {1u, 4u}) {
    eval::EngineOptions eo;
    eo.threads = threads;
    eval::Engine engine(kSettings, eo);
    const auto ev = engine.evaluate(project);
    INFO(bracket::codes(ev.diagnostics));
    CHECK(ev.ok());
    const auto& r = result(ev, *id);
    CHECK(r.status == VolumeStatus::Current);
    REQUIRE(r.contents);
    REQUIRE(r.contents->mesh);
    CHECK(r.contents->mesh->triangle_count() == 20);
    REQUIRE(r.contents->outputs.size() == 2);
    CHECK(std::get<double>(*r.contents->outputs[0].value) > 0.0);
    hashes.push_back(ev.output_hash());
  }
  CHECK(hashes[0] == hashes[1]);
}

TEST_CASE("Imported contents that stick out of their boundary fail containment",
          "[eval][import]") {
  auto r = scenes::row(1);  // V0: the box (0,0,0)-(20,20,20)
  eval::Engine engine(kSettings);
  const auto good = engine.evaluate(r.project);
  REQUIRE(good.ok());

  SECTION("STEP") {
    const auto file = bracket::step_box(0, 0, 0, 20, 20, 25);  // 5 mm out of the top
    REQUIRE_FALSE(eval::import_contents(r.project, r.volumes[0], file, "step", kSettings)
                      .has_errors());
    const auto ev = engine.evaluate(r.project);
    const auto& v = result(ev, r.volumes[0]);
    CHECK(v.status == VolumeStatus::Failed);
    CHECK(bracket::codes(v.diagnostics).find("validate.containment") != std::string::npos);
    CHECK(v.contents == good.volumes[0].contents);  // the last valid contents, stale
    CHECK(v.stale_contents);
  }
  SECTION("STEP that fits") {
    const auto file = bracket::step_box(1, 1, 1, 19, 19, 19);
    REQUIRE_FALSE(eval::import_contents(r.project, r.volumes[0], file, "stp", kSettings)
                      .has_errors());
    const auto ev = engine.evaluate(r.project);
    CHECK(result(ev, r.volumes[0]).status == VolumeStatus::Current);
  }
  SECTION("a mesh") {
    const auto file = read_file(std::filesystem::path(VS_MESH_TEST_DATA) / "hex_prism.stl");
    REQUIRE_FALSE(eval::import_contents(r.project, r.volumes[0], file, "stl", kSettings)
                      .has_errors());
    const auto ev = engine.evaluate(r.project);
    const auto& v = result(ev, r.volumes[0]);
    // The hex prism doesn't sit inside this box.
    CHECK(v.status == VolumeStatus::Failed);
    CHECK(bracket::codes(v.diagnostics).find("validate.containment") != std::string::npos);
  }
}

TEST_CASE("Imports that can't work are refused before anything changes", "[eval][import]") {
  auto r = scenes::row(1);
  const auto before = r.project;
  const model::Blob junk{std::byte{'x'}, std::byte{'y'}};
  CHECK(eval::import_volume(r.project, junk, "dwg", "X", kSettings).error().code ==
        "eval.import.format");
  CHECK(eval::import_volume(r.project, junk, "step", "X", kSettings).error().code ==
        "eval.import.read");
  CHECK(bracket::codes(eval::import_contents(r.project, core::Id::random(), junk, "step",
                                             kSettings)) == "eval.import.volume ");
  CHECK(r.project == before);
}

#include "samples.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <fstream>
#include <iterator>

#include <catch2/catch_test_macros.hpp>

#include "vs/core/settings.hpp"
#include "vs/mesh/import.hpp"
#include "vs/proc/generator.hpp"

namespace samples {

std::filesystem::path source_dir() { return std::filesystem::path(VS_SOURCE_DIR); }

std::vector<std::filesystem::path> projects() {
  std::vector<std::filesystem::path> out;
  for (const auto& lib : std::filesystem::directory_iterator(source_dir() / "libs")) {
    const auto data = lib.path() / "tests" / "data";
    if (!std::filesystem::is_directory(data)) continue;
    for (const auto& entry : std::filesystem::directory_iterator(data)) {
      if (entry.path().extension() == ".vsproj") out.push_back(entry.path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

vs::model::Project load(const std::string& name) {
  for (const auto& p : projects()) {
    if (p.filename() == name) {
      auto loaded = vs::model::load_project(p, false);
      INFO(name);
      REQUIRE(loaded.ok());
      return std::move(loaded).value();
    }
  }
  FAIL("no sample project " << name);
  return {};
}

vs::eval::Evaluation evaluate(const vs::model::Project& project) {
  vs::eval::Engine engine(vs::core::ProductSettings::defaults());
  vs::proc::install(engine);
  return engine.evaluate(project);
}

vs::model::Project long_procedural(double cell) {
  vs::model::Project project = load("shell_and_ball.vsproj");
  auto& graph = std::get<vs::model::ProceduralContents>(project.document.volumes.at(0).contents).graph;
  const std::string from = "\"cell_size\": 1.0";
  const auto at = graph.find(from);
  REQUIRE(at != std::string::npos);
  graph.replace(at, from.size(), "\"cell_size\": " + std::to_string(cell));
  return project;
}

vs::model::Project static_mesh() {
  vs::model::Project project;
  project.document = vs::model::make_document("Imported hex prism");
  std::ifstream f(source_dir() / "libs" / "vs_mesh" / "tests" / "data" / "hex_prism.stl", std::ios::binary);
  REQUIRE(f);
  const std::vector<char> chars((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(chars.size());
  std::transform(chars.begin(), chars.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
  const auto id = vs::mesh::import_mesh_volume(project, bytes, "stl", "Hex", vs::core::ProductSettings::defaults());
  REQUIRE(id.ok());
  return project;
}

bool wait_until(const std::function<bool()>& done, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (!done()) {
    if (timer.elapsed() > timeout_ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  return true;
}

std::string codes(const vs::core::Diagnostics& diagnostics) {
  std::string out;
  for (const auto& d : diagnostics) out += d.to_string() + "\n";
  return out;
}

}  // namespace samples

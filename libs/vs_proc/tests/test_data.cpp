// The sample files under tests/data, used by the CLI tests:
//   shell_and_ball.vgraph.json   the acceptance graph
//   loop.vgraph.json             a graph whose edges form a loop
//   shell_and_ball.vsproj        one procedural box volume, "Shelled", running it
// Regenerate them with `vs_proc_tests "[.generate]"`.

#include <filesystem>
#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/proc/check.hpp"
#include "vs/proc/generator.hpp"

namespace fs = std::filesystem;
using namespace vs;
using namespace vs::proc;

namespace {

const fs::path kData = VS_PROC_TEST_DATA;

std::string read(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

void write(const fs::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary);
  out << text;
  REQUIRE(out.good());
}

Graph loop_graph() {
  Graph g;
  const Id a = g.add("math.add", "A");
  const Id b = g.add("math.multiply", "B");
  const Id c = g.add("field.constant", "C");
  const Id out = g.add("output", "Output");
  g.link(a, "value", b, "a");
  g.link(b, "value", a, "a");
  g.link(b, "value", c, "value");
  g.link(c, "field", out, "contents");
  return g;
}

}  // namespace

TEST_CASE("Write the sample graphs and project", "[.generate]") {
  fs::create_directories(kData);
  write(kData / "shell_and_ball.vgraph.json", to_json(scenes::shell_and_ball().graph) + "\n");
  write(kData / "loop.vgraph.json", to_json(loop_graph()) + "\n");
  auto b = scenes::boxes({scenes::shell_and_ball().graph});
  b.project.document.name = "Shell and ball";
  b.project.document.volumes[0].name = "Shelled";
  const fs::path project = kData / "shell_and_ball.vsproj";
  std::error_code ec;
  fs::remove_all(project, ec);
  REQUIRE_FALSE(model::save_project(b.project, project, model::ProjectLayout::Folder).has_errors());
}

TEST_CASE("The sample files read, check and evaluate", "[proc][data]") {
  const auto good = graph_from_json(read(kData / "shell_and_ball.vgraph.json"));
  REQUIRE(good);
  CHECK(check_graph(*good).empty());
  const auto loop = graph_from_json(read(kData / "loop.vgraph.json"));
  REQUIRE(loop);
  CHECK_FALSE(check_graph(*loop).with_code("proc.graph.loop").empty());

  const auto project = model::load_project(kData / "shell_and_ball.vsproj");
  REQUIRE(project);
  eval::Engine engine(core::ProductSettings::defaults());
  install(engine);
  const auto ev = engine.evaluate(*project);
  INFO(scenes::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  const FieldContents* fc = field_contents(ev.volumes.at(0));
  REQUIRE(fc != nullptr);
  CHECK(fc->samples.grid.nx == 41);
  CHECK(fc->samples.at(0, 15, 10) == 0.0f);  // on the -x face
}

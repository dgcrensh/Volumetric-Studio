// The sample files under tests/data, used by the CLI tests:
//   shell_and_ball.vgraph.json   the acceptance graph
//   loop.vgraph.json             a graph whose edges form a loop
//   shell_and_ball.vsproj        one procedural box volume, "Shelled", running it
//   templates/<name>.vgraph.json the M12 starter templates (bolt_bosses on
//                                bolt_bosses.vsproj's flange)
//   bolt_bosses.vsproj           a procedural box ("Bosses", the bolt_bosses
//                                template at 1 mm) bolted to a parametric one
// Regenerate them with `vs_proc_tests "[.generate]"`.

#include <filesystem>
#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/proc/check.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/templates.hpp"

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

  // The bolt-boss scene first: the templates' surface is its flange.
  scenes::Mixed m = scenes::mixed(scenes::shell_and_ball().graph, true);
  m.project.document.name = "Bolt bosses";
  model::Volume& bosses = *m.project.document.find_volume(m.procedural);
  bosses.name = "Bosses";
  TemplateOptions coarse;
  coarse.surface = m.surface;
  coarse.cell_size = 1.0;
  bosses.contents = model::ProceduralContents{to_json(*make_template("bolt_bosses", coarse))};
  bosses.outputs = {{core::Id::random(), "boss_count", model::OutputQuantity::Published, 1},
                    {core::Id::random(), "mass", model::OutputQuantity::Mass, 1.24}};
  const fs::path flange = kData / "bolt_bosses.vsproj";
  fs::remove_all(flange, ec);
  REQUIRE_FALSE(model::save_project(m.project, flange, model::ProjectLayout::Folder).has_errors());

  fs::create_directories(kData / "templates");
  TemplateOptions o;
  o.surface = m.surface;
  for (const auto& t : templates()) {
    write(kData / "templates" / (t.name + ".vgraph.json"), to_json(*make_template(t.name, o)) + "\n");
  }
}

TEST_CASE("The saved templates are what make_template() gives", "[proc][data][templates]") {
  const auto project = model::load_project(kData / "bolt_bosses.vsproj");
  REQUIRE(project);
  TemplateOptions o;
  o.surface = project->document.connections.at(0).id;
  for (const auto& t : templates()) {
    INFO(t.name);
    const auto saved = graph_from_json(read(kData / "templates" / (t.name + ".vgraph.json")));
    REQUIRE(saved);
    CHECK(to_json(*saved) == to_json(*make_template(t.name, o)));
    CHECK_FALSE(check_graph(*saved).has_errors());
  }
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

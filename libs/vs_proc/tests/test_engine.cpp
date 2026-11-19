#include <set>

#include <catch2/catch_test_macros.hpp>

#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/proc/generator.hpp"
#include "vs/validate/registry.hpp"

using namespace vs;
using namespace vs::proc;
using model::VolumeStatus;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

eval::EngineOptions threads(std::size_t n) {
  eval::EngineOptions o;
  o.threads = n;
  return o;
}

std::set<std::string> regenerated(const eval::Evaluation& ev) {
  std::set<std::string> out;
  for (std::size_t i = 0; i < ev.volumes.size(); ++i) {
    if (ev.volumes[i].regenerated) out.insert(ev.document.volumes[i].name);
  }
  return out;
}

void set_wall(scenes::Boxes& b, std::size_t volume, double value) {
  for (auto& p : b.project.document.volumes[volume].parameters) {
    if (p.id == b.wall[volume]) p.value = feature::Param::of(value);
  }
}

void set_graph(scenes::Boxes& b, std::size_t volume, const Graph& g) {
  std::get<model::ProceduralContents>(b.project.document.volumes[volume].contents).graph = to_json(g);
}

}  // namespace

TEST_CASE("Without a generator procedural volumes stay stale", "[proc][engine]") {
  auto b = scenes::boxes({scenes::shell_and_ball().graph});
  eval::Engine engine(kSettings, threads(2));
  const auto ev = engine.evaluate(b.project);
  CHECK(ev.volumes[0].status == VolumeStatus::Stale);
  CHECK_FALSE(ev.volumes[0].regenerated);
}

TEST_CASE("Procedural volumes regenerate only when their inputs change", "[proc][engine]") {
  const auto s = scenes::shell_and_ball();
  auto b = scenes::boxes({s.graph, s.graph, s.graph});
  eval::Engine engine(kSettings, threads(4));
  const auto gen = install(engine);
  const auto first = engine.evaluate(b.project);
  INFO(scenes::codes(first.diagnostics));
  REQUIRE(first.ok());
  CHECK(regenerated(first) == std::set<std::string>{"P0", "P1", "P2"});

  const auto again = engine.evaluate(b.project);
  CHECK(regenerated(again).empty());
  CHECK(again.stats.cache_hits == 3);
  CHECK(again.output_hash() == first.output_hash());

  set_wall(b, 1, 3.0);
  const auto changed = engine.evaluate(b.project);
  REQUIRE(changed.ok());
  CHECK(regenerated(changed) == std::set<std::string>{"P1"});
  CHECK(changed.output_hash() != first.output_hash());
  (void)gen;
}

TEST_CASE("Graph evaluation is cached per node by input hash", "[proc][engine]") {
  const auto s = scenes::shell_and_ball();
  auto b = scenes::boxes({s.graph});
  eval::Engine engine(kSettings, threads(2));
  const auto gen = install(engine);
  REQUIRE(engine.evaluate(b.project).ok());
  // Every node ran once: 6 nodes.
  CHECK(gen->nodes_evaluated() == 6);
  CHECK(gen->node_cache_hits() == 0);

  // The wall feeds only the shell and what is below it. The input nodes
  // always run (2), the ball is cached (1 hit), and the shell, union and
  // output run again.
  set_wall(b, 0, 3.0);
  REQUIRE(engine.evaluate(b.project).ok());
  CHECK(gen->nodes_evaluated() == 6 + 5);
  CHECK(gen->node_cache_hits() == 1);

  // Moving the ball: the shell's inputs are unchanged, so it is cached.
  Graph g = s.graph;
  g.find_node(s.ball)->settings["radius"] = 5.0;
  set_graph(b, 0, g);
  REQUIRE(engine.evaluate(b.project).ok());
  CHECK(gen->nodes_evaluated() == 6 + 5 + 5);  // inputs 2, ball, union, output
  CHECK(gen->node_cache_hits() == 1 + 1);     // the shell
}

TEST_CASE("A graph with a type error fails its volume only, at the node",
          "[proc][engine]") {
  const auto s = scenes::shell_and_ball();
  Graph bad = s.graph;
  for (auto& e : bad.edges) {
    if (e.to.node == s.shell && e.to.port == "thickness") e.from = {s.ball, "field"};
  }
  auto b = scenes::boxes({s.graph, bad, s.graph});
  eval::Engine engine(kSettings, threads(4));
  install(engine);
  const auto ev = engine.evaluate(b.project);
  CHECK(ev.volumes[0].status == VolumeStatus::Current);
  CHECK(ev.volumes[1].status == VolumeStatus::Failed);
  CHECK(ev.volumes[2].status == VolumeStatus::Current);
  const auto mismatch = ev.diagnostics.with_code("proc.graph.type_mismatch");
  REQUIRE(mismatch.size() == 1);
  CHECK(mismatch.front()->subjects.front().id == s.shell);
  const auto target = validate::route(*mismatch.front(), ev.document);
  CHECK(target.on == validate::ActOn::Node);
  CHECK(target.subject.id == s.shell);
  CHECK(target.volume == b.volumes[1]);
}

TEST_CASE("An infeasible node fails its volume, which keeps its last contents",
          "[proc][engine]") {
  const auto s = scenes::shell_and_ball();
  auto b = scenes::boxes({s.graph, s.graph});
  eval::Engine engine(kSettings, threads(2));
  install(engine);
  const auto first = engine.evaluate(b.project);
  REQUIRE(first.ok());

  set_wall(b, 0, -1.0);  // a negative shell thickness
  const auto ev = engine.evaluate(b.project);
  CHECK(ev.volumes[0].status == VolumeStatus::Failed);
  CHECK(ev.volumes[0].stale_contents);
  CHECK(field_contents(ev.volumes[0]) == field_contents(first.volumes[0]));
  CHECK(ev.volumes[1].status == VolumeStatus::Current);
  const auto infeasible = ev.diagnostics.with_code("proc.node.infeasible");
  REQUIRE(infeasible.size() == 1);
  CHECK(infeasible.front()->subjects.front().id == s.shell);
  CHECK(infeasible.front()->subjects.at(1).label == "Shell.thickness");
  CHECK(infeasible.front()->subjects.back().id == b.volumes[0]);
}

TEST_CASE("Graphs read their connection surfaces", "[proc][engine]") {
  auto b = scenes::boxes({scenes::shell_and_ball().graph, scenes::shell_and_ball().graph});
  Graph g;
  const Id surface = g.add("input.surface", "Joint", {{"surface", Setting(b.surfaces[0].to_string())}});
  const Id near = g.add("query.distance_to_surface", "Near",
                        {{"surface", Setting(b.surfaces[0].to_string())}});
  const Id slab = g.add("field.offset", "Slab", {{"distance", Setting(1.0)}});
  const Id cut = g.add("field.intersection", "Within");
  const Id out = g.add("output", "Output", {{"cell_size", Setting(2.0)}});
  g.link(near, "distance", slab, "field");
  g.link(slab, "field", cut, "a");
  g.link(surface, "plane", cut, "b");
  g.link(cut, "field", out, "contents");
  set_graph(b, 0, g);

  eval::Engine engine(kSettings, threads(2));
  install(engine);
  const auto ev = engine.evaluate(b.project);
  INFO(scenes::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  const FieldContents* fc = field_contents(ev.volumes[0]);
  REQUIRE(fc != nullptr);
  // The patch: 10 x 10 on x = 40 round (40, 10, 10). The plane's normal
  // points along +x or -x; either way, 0.5 mm from the patch centre on the
  // P0 side is within the 1 mm slab.
  const auto& f = *fc->field;
  CHECK(f.eval({39.5, 10, 10}) < 0.0);
  CHECK(f.eval({30, 10, 10}) > 0.0);   // too far
  CHECK(f.eval({39.5, 25, 10}) > 0.0);  // off the patch

  // A graph reading a surface that doesn't join its volume fails.
  auto c = scenes::boxes({scenes::shell_and_ball().graph, scenes::shell_and_ball().graph,
                          scenes::shell_and_ball().graph});
  Graph peek = scenes::shell_and_ball().graph;
  peek.add("input.surface", "Peek", {{"surface", Setting(c.surfaces[1].to_string())}});
  set_graph(c, 0, peek);
  eval::Engine e2(kSettings, threads(2));
  install(e2);
  const auto ev2 = e2.evaluate(c.project);
  CHECK(ev2.volumes[0].status == VolumeStatus::Failed);
  CHECK_FALSE(ev2.diagnostics.with_code("proc.reference.surface").empty());
}

TEST_CASE("Broken graph JSON fails the volume", "[proc][engine]") {
  auto b = scenes::boxes({scenes::shell_and_ball().graph});
  std::get<model::ProceduralContents>(b.project.document.volumes[0].contents).graph = "{nope";
  eval::Engine engine(kSettings, threads(2));
  install(engine);
  const auto ev = engine.evaluate(b.project);
  CHECK(ev.volumes[0].status == VolumeStatus::Failed);
  const auto parse = ev.diagnostics.with_code("proc.json.parse");
  REQUIRE(parse.size() == 1);
  const auto target = validate::route(*parse.front(), ev.document);
  CHECK(target.on == validate::ActOn::Contents);
  CHECK(target.volume == b.volumes[0]);
}

TEST_CASE("Field contents measure nothing yet", "[proc][engine]") {
  auto b = scenes::boxes({scenes::shell_and_ball().graph});
  eval::Engine engine(kSettings, threads(1));
  install(engine);
  const auto ev = engine.evaluate(b.project);
  REQUIRE(ev.ok());
  const auto& c = ev.volumes[0].contents;
  REQUIRE(c);
  REQUIRE(c->outputs.size() == 1);
  CHECK_FALSE(c->outputs[0].value.has_value());
  CHECK(c->solid.is_empty());
  CHECK_FALSE(c->mesh.has_value());
}

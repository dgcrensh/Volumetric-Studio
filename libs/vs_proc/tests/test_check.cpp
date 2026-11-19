#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "scenes.hpp"
#include "vs/proc/check.hpp"

using namespace vs;
using namespace vs::proc;
using core::Diagnostics;

namespace {

bool has(const Diagnostics& d, std::string_view code) { return !d.with_code(code).empty(); }

}  // namespace

TEST_CASE("The acceptance graph checks clean", "[proc][check]") {
  const auto s = scenes::shell_and_ball();
  const auto d = check_graph(s.graph);
  INFO(scenes::codes(d));
  CHECK(d.empty());
  const auto order = topological_order(s.graph);
  REQUIRE(order.size() == s.graph.nodes.size());
  // Every node after the nodes feeding it.
  const auto pos = [&](const core::Id& id) {
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (s.graph.nodes[order[i]].id == id) return i;
    }
    return order.size();
  };
  for (const auto& e : s.graph.edges) CHECK(pos(e.from.node) < pos(e.to.node));
}

TEST_CASE("Type errors are rejected when authored", "[proc][check][acceptance]") {
  auto s = scenes::shell_and_ball();
  Graph g = s.graph;
  const Id number = g.add("math.add", "Number");
  const Graph with_number = g;
  // A scalar into a field input.
  auto d = connect(g, {number, "value"}, {s.join, "radius"});
  CHECK(has(d, "proc.graph.unknown_port"));  // a union has no radius
  CHECK(g == with_number);
  // Disconnect the sphere and try to feed the union a scalar instead.
  std::erase_if(g.edges, [&](const Edge& e) { return e.to.node == s.join && e.to.port == "b"; });
  const Graph unfinished = g;
  d = connect(g, {number, "value"}, {s.join, "b"});
  REQUIRE(has(d, "proc.graph.type_mismatch"));
  const auto* mismatch = d.with_code("proc.graph.type_mismatch").front();
  CHECK(mismatch->subjects.at(0).id == s.join);
  CHECK(mismatch->subjects.at(1).id == number);
  CHECK(g == unfinished);  // rejected: the graph is unchanged
  // A field into a scalar input.
  d = connect(g, {s.ball, "field"}, {number, "a"});
  CHECK(has(d, "proc.graph.type_mismatch"));
  CHECK(g == unfinished);
  // The right type is accepted, though the graph is still unfinished
  // elsewhere.
  d = connect(g, {s.ball, "field"}, {s.join, "b"});
  CHECK_FALSE(has(d, "proc.graph.type_mismatch"));
  CHECK(g != unfinished);
}

TEST_CASE("Loops are rejected when authored", "[proc][check][acceptance]") {
  Graph g;
  const Id a = g.add("math.add", "A");
  const Id b = g.add("math.multiply", "B");
  const Id c = g.add("math.negate", "C");
  CHECK_FALSE(has(connect(g, {a, "value"}, {b, "a"}), "proc.graph.loop"));
  CHECK_FALSE(has(connect(g, {b, "value"}, {c, "a"}), "proc.graph.loop"));
  REQUIRE(g.edges.size() == 2);
  const Graph before = g;
  const auto d = connect(g, {c, "value"}, {a, "a"});
  REQUIRE(has(d, "proc.graph.loop"));
  CHECK(g == before);
  const auto* loop = d.with_code("proc.graph.loop").front();
  REQUIRE(loop->subjects.size() == 3);
  CHECK(loop->subjects[0].id == a);
  CHECK(loop->subjects[1].id == b);
  CHECK(loop->subjects[2].id == c);
  // A node feeding itself.
  CHECK(has(connect(g, {a, "value"}, {a, "b"}), "proc.graph.loop"));
  CHECK(g == before);
  CHECK(topological_order(g).size() == 3);
}

TEST_CASE("Unfinished graphs can be authored but not run", "[proc][check]") {
  Graph g;
  const auto d = edit(g, [](Graph& x) { x.add("field.union", "Join"); });
  CHECK(has(d, "proc.graph.missing_input"));
  CHECK(has(d, "proc.graph.output"));
  CHECK(g.nodes.size() == 1);  // kept
  CHECK(is_unfinished("proc.graph.output"));
  CHECK_FALSE(is_unfinished("proc.graph.loop"));
}

TEST_CASE("Structural problems are reported", "[proc][check]") {
  auto s = scenes::shell_and_ball();
  SECTION("duplicate ids") {
    s.graph.nodes.push_back(s.graph.nodes.front());
    CHECK(has(check_graph(s.graph), "proc.graph.duplicate_id"));
  }
  SECTION("unknown types") {
    s.graph.add("field.teapot");
    CHECK(has(check_graph(s.graph), "proc.graph.unknown_type"));
  }
  SECTION("edges to nowhere") {
    s.graph.link(core::Id::random(), "field", s.join, "a");
    CHECK(has(check_graph(s.graph), "proc.graph.unknown_node"));
  }
  SECTION("two edges into one input") {
    s.graph.link(s.boundary, "field", s.join, "a");
    CHECK(has(check_graph(s.graph), "proc.graph.input_taken"));
  }
  SECTION("a second output") {
    s.graph.add("output");
    const auto d = check_graph(s.graph);
    CHECK(has(d, "proc.graph.output"));
    CHECK(has(d, "proc.graph.missing_input"));
  }
  SECTION("settings") {
    s.graph.find_node(s.ball)->settings["radius"] = std::string("big");
    s.graph.find_node(s.ball)->settings["colour"] = 3.0;
    s.graph.find_node(s.output)->settings["cell_size"] = core::dvec3{1, 1, 1};
    const auto d = check_graph(s.graph);
    CHECK(d.with_code("proc.graph.setting").size() == 3);
  }
  SECTION("required properties and choices") {
    s.graph.find_node(s.wall)->settings.clear();
    const Id cmp = s.graph.add("logic.compare", "Compare", {{"op", Setting(std::string("about"))}});
    const auto d = check_graph(s.graph);
    CHECK(d.with_code("proc.graph.setting").size() == 2);
    (void)cmp;
  }
  SECTION("parameters") {
    s.graph.find_node(s.wall)->settings["name"] = std::string("thickness");
    s.graph.parameters.push_back({"wall", 3});
    s.graph.parameters.push_back({"2bad", 3});
    CHECK(check_graph(s.graph).with_code("proc.graph.parameter").size() == 3);
  }
}

TEST_CASE("A graph may read only the surfaces joining its volume", "[proc][check]") {
  auto b = scenes::boxes({scenes::shell_and_ball().graph, scenes::shell_and_ball().graph,
                          scenes::shell_and_ball().graph});
  Graph g = scenes::shell_and_ball().graph;
  g.add("input.surface", "Joint", {{"surface", Setting(b.surfaces[1].to_string())}});
  // S1 joins P1 and P2, not P0.
  CHECK(check_volume_graph(g, b.project.document, b.volumes[1]).empty());
  const auto d = check_volume_graph(g, b.project.document, b.volumes[0]);
  REQUIRE(has(d, "proc.reference.surface"));
  CHECK(d.with_code("proc.reference.surface").front()->subjects.front().kind ==
        core::SubjectKind::Node);
  g.nodes.back().settings["surface"] = core::Id::random().to_string();
  CHECK(has(check_volume_graph(g, b.project.document, b.volumes[1]), "proc.reference.surface"));
}

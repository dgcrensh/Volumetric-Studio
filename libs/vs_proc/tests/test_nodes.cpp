#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "scenes.hpp"
#include "vs/proc/check.hpp"
#include "vs/proc/evaluate.hpp"
#include "vs/proc/field.hpp"

using namespace vs;
using namespace vs::proc;

namespace {

// Evaluates `g` (whose contents are a field.constant) and returns the
// constant's value, or nullopt with the diagnostics in `out`.
std::optional<double> run(const Graph& g, std::uint64_t seed = 0,
                          core::Diagnostics* out = nullptr) {
  const auto in = scenes::bare_inputs(seed);
  const auto ev = evaluate_graph(g, in);
  if (out) *out = ev.diagnostics;
  if (!ev.ok()) return std::nullopt;
  return (*ev.contents)->eval({0, 0, 0});
}

Graph binary(const std::string& type, double a, double b) {
  Graph g;
  const Id n = g.add(type, type, {{"a", Setting(a)}, {"b", Setting(b)}});
  scenes::constant_output(g, n);
  return g;
}

}  // namespace

TEST_CASE("Scalar maths", "[proc][nodes]") {
  CHECK(run(binary("math.add", 2, 3)) == 5.0);
  CHECK(run(binary("math.subtract", 2, 3)) == -1.0);
  CHECK(run(binary("math.multiply", 2, 3)) == 6.0);
  CHECK(run(binary("math.divide", 3, 2)) == 1.5);
  CHECK(run(binary("math.min", 2, 3)) == 2.0);
  CHECK(run(binary("math.max", 2, 3)) == 3.0);
  Graph g;
  const Id sq = g.add("math.sqrt", "Root", {{"a", Setting(16.0)}});
  scenes::constant_output(g, sq);
  CHECK(run(g) == 4.0);
}

TEST_CASE("Logic", "[proc][nodes]") {
  Graph g;
  const Id cmp = g.add("logic.compare", "Cmp",
                       {{"a", Setting(1.0)}, {"b", Setting(2.0)}, {"op", Setting(std::string("less"))}});
  const Id sel = g.add("logic.select", "Pick", {{"a", Setting(10.0)}, {"b", Setting(20.0)}});
  g.link(cmp, "value", sel, "condition");
  scenes::constant_output(g, sel);
  CHECK(run(g) == 10.0);
  g.find_node(cmp)->settings["op"] = std::string("greater_equal");
  CHECK(run(g) == 20.0);
  CHECK(run(binary("logic.and", 1, 0)) == 0.0);
  CHECK(run(binary("logic.or", 1, 0)) == 1.0);
}

TEST_CASE("Vectors and lists", "[proc][nodes]") {
  Graph g;
  const Id v = g.add("vector.compose", "V", {{"x", Setting(3.0)}, {"y", Setting(4.0)}});
  const Id len = g.add("vector.length", "Len");
  const Id range = g.add("list.range", "Range", {{"start", Setting(1.0)}, {"count", Setting(4.0)}});
  const Id sum = g.add("list.sum", "Sum");
  const Id add = g.add("math.add", "Add");
  g.link(v, "vector", len, "vector");
  g.link(range, "list", sum, "list");
  g.link(len, "value", add, "a");
  g.link(sum, "value", add, "b");
  scenes::constant_output(g, add);
  CHECK(run(g) == 5.0 + (1 + 2 + 3 + 4));

  Graph h;
  const Id r = h.add("list.range", "Range", {{"count", Setting(3.0)}, {"step", Setting(0.5)}});
  const Id get = h.add("list.get", "Get", {{"index", Setting(2.0)}});
  h.link(r, "list", get, "list");
  scenes::constant_output(h, get);
  CHECK(run(h) == 1.0);
  h.find_node(get)->settings["index"] = 3.0;
  core::Diagnostics d;
  CHECK_FALSE(run(h, 0, &d));
  CHECK_FALSE(d.with_code("proc.node.infeasible").empty());
}

TEST_CASE("Infeasible inputs name the node and the inputs", "[proc][nodes]") {
  core::Diagnostics d;
  CHECK_FALSE(run(binary("math.divide", 1, 0), 0, &d));
  REQUIRE(d.size() == 1);
  const auto& diag = d[0];
  CHECK(diag.code == "proc.node.infeasible");
  REQUIRE(diag.subjects.size() == 3);
  CHECK(diag.subjects[0].kind == core::SubjectKind::Node);
  CHECK(diag.subjects[1].label == "math.divide.b");
  CHECK(diag.subjects[2].kind == core::SubjectKind::Volume);

  Graph g;
  const Id clamp = g.add("math.clamp", "Clamp", {{"min", Setting(5.0)}, {"max", Setting(1.0)}});
  scenes::constant_output(g, clamp);
  CHECK_FALSE(run(g, 0, &d));
  REQUIRE(d.size() == 1);
  CHECK(d[0].subjects.size() == 4);  // node, min, max, volume

  Graph s;
  const Id ball = s.add("field.sphere", "Ball", {{"radius", Setting(-1.0)}});
  const Id out = s.add("output");
  s.link(ball, "field", out, "contents");
  CHECK_FALSE(run(s, 0, &d));
  CHECK(d[0].code == "proc.node.infeasible");
  CHECK(d[0].subjects[0].id == ball);
}

TEST_CASE("Random numbers come from the volume's seed and the node's id", "[proc][nodes]") {
  Graph g;
  const Id rnd = g.add("math.random", "Dice", {{"min", Setting(10.0)}, {"max", Setting(20.0)}});
  scenes::constant_output(g, rnd);
  const auto a = run(g, 1);
  REQUIRE(a);
  CHECK(*a >= 10.0);
  CHECK(*a < 20.0);
  CHECK(run(g, 1) == a);  // deterministic
  CHECK(run(g, 2) != a);  // another seed
  Graph h = g;
  h.find_node(rnd)->id = core::Id::random();
  for (auto& e : h.edges) {
    if (e.from.node == rnd) e.from.node = h.find_node("Dice")->id;
  }
  CHECK(run(h, 1) != a);  // another node
  CHECK(derive_seed(1, rnd) != derive_seed(2, rnd));
  CHECK(uniform(5, 0) != uniform(5, 1));
}

TEST_CASE("Every library node declares well-formed ports", "[proc][nodes]") {
  for (const NodeType* t : library().types()) {
    INFO(t->type);
    CHECK(t->evaluate);
    for (const auto& p : t->inputs) {
      if (p.default_value) {
        CHECK((p.type == PortType::Scalar || p.type == PortType::Vector));
      }
    }
    // A node with only defaulted inputs and no required property runs.
    bool simple = true;
    for (const auto& p : t->inputs) simple = simple && p.default_value.has_value();
    for (const auto& p : t->properties) simple = simple && p.default_value.has_value();
    if (!simple || t->type == "output" || t->category == "input" || t->category == "query") continue;
    Graph g;
    const Id n = g.add(t->type, t->type);
    const Id ball = g.add("field.sphere");
    const Id out = g.add("output");
    g.link(ball, "field", out, "contents");
    const auto ev = evaluate_graph(g, scenes::bare_inputs());
    INFO(scenes::codes(ev.diagnostics));
    CHECK(ev.ok());
    CHECK(ev.find(n) != nullptr);
  }
  CHECK(library().find("field.sphere") != nullptr);
  CHECK(library().find("field.teapot") == nullptr);
}

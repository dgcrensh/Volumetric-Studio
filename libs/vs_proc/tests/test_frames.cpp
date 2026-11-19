// M12 frames: scatter (seeded points in the volume or on a connection
// surface) and patterns (linear, circular, grid).

#include <cmath>
#include <numbers>
#include <set>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "scenes.hpp"
#include "vs/model/build.hpp"
#include "vs/proc/evaluate.hpp"
#include "vs/proc/field.hpp"
#include "vs/proc/inspect.hpp"

using namespace vs;
using namespace vs::proc;
using core::dvec3;
using Catch::Approx;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

// The frames a node makes, inspected on its own.
std::vector<core::ftransform> frames_of(const Graph& g, const Id& node, const GraphInputs& in,
                                        core::Diagnostics* problems = nullptr) {
  const NodeInspection ins = inspect_node(g, in, node);
  if (problems != nullptr) *problems = ins.diagnostics;
  if (!ins.ok()) return {};
  const Value* v = ins.find("frames");
  REQUIRE(v != nullptr);
  return std::get<Frames>(*v)->frames;
}

// The first volume's inputs in a row of two boxes.
struct Row {
  scenes::Boxes boxes;
  model::DocumentBuild layout;
  GraphInputs inputs;
};

Row row(std::uint64_t seed = 0) {
  Row r;
  r.boxes = scenes::boxes({scenes::shell_and_ball().graph, scenes::shell_and_ball().graph});
  r.boxes.project.document.volumes[0].seed = seed;
  r.layout = model::build_layout(r.boxes.project.document, kSettings);
  auto in = gather_inputs(r.boxes.project.document, r.layout, 0, kSettings);
  REQUIRE(in);
  r.inputs = *in;
  return r;
}

dvec3 at(const core::ftransform& f) { return core::to_fp64(f.translation); }

}  // namespace

TEST_CASE("Patterns repeat frames", "[proc][frames]") {
  const GraphInputs in = scenes::bare_inputs();
  SECTION("linear") {
    Graph g;
    const Id n = g.add("pattern.linear", "Row", {{"count", Setting(3.0)}, {"step", Setting(dvec3{0, 2, 0})}});
    const auto f = frames_of(g, n, in);
    REQUIRE(f.size() == 3);
    CHECK(at(f[2]) == dvec3{0, 4, 0});
  }
  SECTION("circular, a full turn") {
    Graph g;
    const Id start = g.add("frames.point", "Start", {{"position", Setting(dvec3{10, 0, 5})}});
    const Id n = g.add("pattern.circular", "Ring", {{"count", Setting(4.0)}});
    g.link(start, "frames", n, "frames");
    const auto f = frames_of(g, n, in);
    REQUIRE(f.size() == 4);
    CHECK(at(f[1]).x == Approx(0.0).margin(1e-6));
    CHECK(at(f[1]).y == Approx(10.0));
    CHECK(at(f[2]).x == Approx(-10.0));
    CHECK(at(f[3]).y == Approx(-10.0));
    CHECK(at(f[3]).z == Approx(5.0));
    // The frame turns with the pattern: its x axis now points along -x at f[2].
    CHECK(f[2].linear.cols[0].x == Approx(-1.0f));
  }
  SECTION("circular, an arc ends at the angle") {
    Graph g;
    const Id start = g.add("frames.point", "Start", {{"position", Setting(dvec3{10, 0, 0})}});
    const Id n = g.add("pattern.circular", "Arc",
                       {{"count", Setting(3.0)}, {"angle", Setting(std::numbers::pi / 2)}});
    g.link(start, "frames", n, "frames");
    const auto f = frames_of(g, n, in);
    REQUIRE(f.size() == 3);
    CHECK(at(f[2]).y == Approx(10.0));
    CHECK(at(f[1]).x == Approx(10.0 * std::sqrt(0.5)));
  }
  SECTION("grid of a merged pair") {
    Graph g;
    const Id a = g.add("frames.point", "A");
    const Id b = g.add("frames.point", "B", {{"position", Setting(dvec3{0, 0, 100})}});
    const Id both = g.add("frames.merge", "Both");
    g.link(a, "frames", both, "a");
    g.link(b, "frames", both, "b");
    const Id n = g.add("pattern.grid", "Grid",
                       {{"counts", Setting(dvec3{2, 3, 1})}, {"spacing", Setting(dvec3{1, 2, 3})}});
    g.link(both, "frames", n, "frames");
    const Id count = g.add("frames.count", "Count");
    g.link(n, "frames", count, "frames");
    const auto f = frames_of(g, n, in);
    REQUIRE(f.size() == 12);
    CHECK(at(f[5]) == dvec3{1, 4, 0});
    CHECK(at(f[11]) == dvec3{1, 4, 100});
    const NodeInspection c = inspect_node(g, in, count);
    REQUIRE(c.ok());
    CHECK(std::get<double>(*c.find("count")) == 12.0);
  }
  SECTION("counts are checked") {
    Graph g;
    const Id n = g.add("pattern.grid", "Grid", {{"counts", Setting(dvec3{2, 0.5, 1})}});
    core::Diagnostics d;
    CHECK(frames_of(g, n, in, &d).empty());
    CHECK_FALSE(d.with_code("proc.node.infeasible").empty());
    Graph big;
    const Id m = big.add("pattern.grid", "Grid", {{"counts", Setting(dvec3{100, 100, 100})}});
    CHECK(frames_of(big, m, in, &d).empty());
    CHECK_FALSE(d.with_code("proc.node.infeasible").empty());
  }
}

TEST_CASE("Scatter in the volume is seeded, inside and spaced", "[proc][frames][scatter]") {
  Graph g;
  const Id s = g.add("scatter.volume", "Points",
                     {{"count", Setting(200.0)}, {"margin", Setting(2.0)}, {"spacing", Setting(2.5)}});
  const Row r = row(1);
  const auto f = frames_of(g, s, r.inputs);
  REQUIRE(f.size() == 200);
  for (std::size_t i = 0; i < f.size(); ++i) {
    const dvec3 p = at(f[i]);
    CHECK(scenes::box_distance(p, {0, 0, 0}, {40, 30, 20}) <= -2.0 + 1e-5);
    for (std::size_t j = 0; j < i; ++j) CHECK(length(p - at(f[j])) >= 2.5 - 1e-5);
  }
  // Same seed, same points; another seed, others.
  CHECK(frames_of(g, s, row(1).inputs).front().translation == f.front().translation);
  CHECK(frames_of(g, s, row(2).inputs).front().translation != f.front().translation);
  // Asking for more than fits is infeasible, not a hang.
  Graph full;
  const Id t = full.add("scatter.volume", "Points", {{"count", Setting(5000.0)}, {"spacing", Setting(5.0)}});
  core::Diagnostics d;
  CHECK(frames_of(full, t, r.inputs, &d).empty());
  CHECK_FALSE(d.with_code("proc.node.infeasible").empty());
}

TEST_CASE("Scatter on a surface lies on its patch, facing along its normal", "[proc][frames][scatter]") {
  const Row r = row();
  Graph g;
  const Id s = g.add("scatter.surface", "Points",
                     {{"surface", Setting(r.boxes.surfaces[0].to_string())}, {"count", Setting(50.0)}});
  const auto f = frames_of(g, s, r.inputs);
  REQUIRE(f.size() == 50);
  for (const auto& t : f) {
    const dvec3 p = at(t);
    CHECK(p.x == Approx(40.0));  // the patch on x = 40, 10 x 10 round (y, z) = (10, 10)
    CHECK(p.y >= 5.0 - 1e-5);
    CHECK(p.y <= 15.0 + 1e-5);
    CHECK(p.z >= 5.0 - 1e-5);
    CHECK(p.z <= 15.0 + 1e-5);
    CHECK(std::abs(t.linear.cols[2].x) == Approx(1.0f));
  }
}

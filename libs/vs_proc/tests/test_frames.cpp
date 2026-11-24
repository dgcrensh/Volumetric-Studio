// M12 frames: scatter (seeded points in the volume or on a connection
// surface) and patterns (linear, circular, grid).

#include <cmath>
#include <numbers>
#include <set>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "scenes.hpp"
#include "vs/model/build.hpp"
#include "vs/proc/check.hpp"
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

TEST_CASE("Offset moves frames along their own z, into the boundary", "[proc][frames][offset]") {
  // Box 0..40 x 0..30 x 0..20; the patch lies on x = 40, its normal +x, so
  // into the boundary is -x.
  const Row r = row();
  Graph g;
  const Id s = g.add("scatter.surface", "Points",
                     {{"surface", Setting(r.boxes.surfaces[0].to_string())}, {"count", Setting(20.0)}});
  const Id o = g.add("frames.offset", "Inward", {{"distance", Setting(3.0)}});
  g.link(s, "frames", o, "frames");
  const auto on = frames_of(g, s, r.inputs);
  REQUIRE(on.size() == 20);

  SECTION("one layer, into the boundary by default") {
    const auto f = frames_of(g, o, r.inputs);
    REQUIRE(f.size() == 20);
    for (std::size_t i = 0; i < f.size(); ++i) {
      CHECK(at(f[i]).x == Approx(37.0));
      CHECK(at(f[i]).y == Approx(at(on[i]).y));
      CHECK(at(f[i]).z == Approx(at(on[i]).z));
      CHECK(f[i].linear.cols[2].x == on[i].linear.cols[2].x);  // rotation kept
    }
  }
  SECTION("along z goes out instead") {
    g.find_node(o)->settings["direction"] = Setting(std::string("along_z"));
    const auto f = frames_of(g, o, r.inputs);
    REQUIRE(f.size() == 20);
    CHECK(at(f[0]).x == Approx(43.0));
  }
  SECTION("layers stack at distance + k step, frame by frame") {
    g.find_node(o)->settings["layers"] = Setting(3.0);
    g.find_node(o)->settings["step"] = Setting(5.0);
    const auto f = frames_of(g, o, r.inputs);
    REQUIRE(f.size() == 60);
    CHECK(at(f[0]).x == Approx(37.0));
    CHECK(at(f[1]).x == Approx(32.0));
    CHECK(at(f[2]).x == Approx(27.0));
    CHECK(at(f[3]).y == Approx(at(on[1]).y));
  }
  SECTION("margin drops points too close to the boundary") {
    // Layers at x = 37, 27, 17 and 7. Points on the patch lie 5 to 15 mm
    // from the y and z faces, so only the first layer (3 mm from x = 40)
    // is less than the 4 mm margin deep.
    g.find_node(o)->settings["layers"] = Setting(4.0);
    g.find_node(o)->settings["step"] = Setting(10.0);
    g.find_node(o)->settings["margin"] = Setting(4.0);
    const auto f = frames_of(g, o, r.inputs);
    REQUIRE(f.size() == 60);
    for (const auto& t : f) {
      CHECK((at(t).x == Approx(27.0) || at(t).x == Approx(17.0) || at(t).x == Approx(7.0)));
    }
  }
  SECTION("a margin nothing satisfies is infeasible") {
    g.find_node(o)->settings["margin"] = Setting(50.0);
    core::Diagnostics problems;
    CHECK(frames_of(g, o, r.inputs, &problems).empty());
    REQUIRE(problems.size() == 1);
    CHECK(problems.items().front().code == "proc.node.infeasible");
  }
  SECTION("bad values are infeasible") {
    core::Diagnostics problems;
    g.find_node(o)->settings["layers"] = Setting(0.0);
    CHECK(frames_of(g, o, r.inputs, &problems).empty());
    REQUIRE(problems.size() == 1);
    CHECK(problems.items().front().code == "proc.node.infeasible");
    g.find_node(o)->settings["layers"] = Setting(1.0);
    g.find_node(o)->settings["margin"] = Setting(-1.0);
    CHECK(frames_of(g, o, r.inputs, &problems).empty());
    REQUIRE(problems.size() == 1);
    CHECK(problems.items().front().code == "proc.node.infeasible");
  }
}

TEST_CASE("Scatter on the boundary lies on its skin, facing out", "[proc][frames][scatter][boundary]") {
  // Box 0..40 x 0..30 x 0..20.
  Row r = row(3);
  Graph g;
  const Id s = g.add("scatter.boundary", "Skin", {{"count", Setting(300.0)}, {"spacing", Setting(1.0)}});

  SECTION("the whole skin, by area") {
    const auto f = frames_of(g, s, r.inputs);
    REQUIRE(f.size() == 300);
    std::set<int> sides;
    for (std::size_t i = 0; i < f.size(); ++i) {
      const dvec3 p = at(f[i]);
      CHECK(std::abs(scenes::box_distance(p, {0, 0, 0}, {40, 30, 20})) <= 1e-4);
      // z points out of the side the point lies on.
      const dvec3 z = core::to_fp64(f[i].linear.cols[2]);
      CHECK(length(z) == Approx(1.0));
      CHECK(scenes::box_distance(p + z, {0, 0, 0}, {40, 30, 20}) == Approx(1.0).margin(1e-4));
      const int side = std::abs(z.x) > 0.5 ? 0 : std::abs(z.y) > 0.5 ? 1 : 2;
      sides.insert(2 * side + ((z.x + z.y + z.z) > 0 ? 1 : 0));
      for (std::size_t j = 0; j < i; ++j) CHECK(length(p - at(f[j])) >= 1.0 - 1e-5);
    }
    CHECK(sides.size() == 6);
    // Seeded.
    CHECK(frames_of(g, s, row(3).inputs).back().translation == f.back().translation);
    CHECK(frames_of(g, s, row(4).inputs).back().translation != f.back().translation);
  }

  SECTION("one named face, gathered because the graph reads it") {
    g.find_node(s)->settings["face"] = Setting(std::string("+z"));
    // Inputs gathered without it don't have it.
    core::Diagnostics problems;
    CHECK(frames_of(g, s, r.inputs, &problems).empty());
    CHECK_FALSE(problems.with_code("proc.reference.boundary_face").empty());

    REQUIRE(boundary_faces_read(g) == std::vector<std::string>{"+z"});
    auto in = gather_inputs(r.boxes.project.document, r.layout, 0, kSettings, {boundary_faces_read(g)});
    REQUIRE(in);
    REQUIRE(in->boundary_faces.size() == 1);
    const auto f = frames_of(g, s, *in);
    REQUIRE(f.size() == 300);
    for (const auto& t : f) {
      CHECK(at(t).z == Approx(20.0));
      CHECK(t.linear.cols[2].z == Approx(1.0f));
    }

    // Through an offset, into the volume.
    const Id o = g.add("frames.offset", "Inward", {{"distance", Setting(2.0)}});
    g.link(s, "frames", o, "frames");
    for (const auto& t : frames_of(g, o, *in)) CHECK(at(t).z == Approx(18.0));
  }

  SECTION("a face the boundary doesn't have") {
    auto in = gather_inputs(r.boxes.project.document, r.layout, 0, kSettings, {{"+w"}});
    REQUIRE_FALSE(in);
    CHECK(in.error().code == "proc.reference.boundary_face");
    g.find_node(s)->settings["face"] = Setting(std::string("+w"));
    const auto checked = check_volume_graph(g, r.boxes.project.document, r.boxes.volumes[0]);
    CHECK_FALSE(checked.with_code("proc.reference.boundary_face").empty());
    g.find_node(s)->settings["face"] = Setting(std::string("-y"));
    CHECK(check_volume_graph(g, r.boxes.project.document, r.boxes.volumes[0])
              .with_code("proc.reference.boundary_face")
              .empty());
  }
}

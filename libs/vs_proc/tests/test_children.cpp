// L4: procedures that emit child volumes: the children nodes, the graph
// rules, and generated layouts through the engine (the same checks as
// authored layouts, the weld, caching and determinism).

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "nested.hpp"
#include "rack.hpp"
#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/check.hpp"
#include "vs/model/generated.hpp"
#include "vs/model/hierarchy.hpp"
#include "vs/proc/check.hpp"
#include "vs/proc/evaluate.hpp"
#include "vs/proc/generator.hpp"

using namespace vs;
using namespace vs::proc;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using core::dbox3;
using core::dvec3;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

struct GridGraph {
  Graph graph;
  Id grid, values, delegate, output;
};

// children.grid over the Rack, Seat passed to the cell on it, and with
// `depths` a list.range of depths (start, step) for the cells.
GridGraph grid_graph(const Id& seat, dvec3 count = {3, 2, 1}, const char* join = "bonded",
                     std::optional<std::pair<double, double>> depths = std::nullopt) {
  GridGraph g;
  g.grid = g.graph.add("children.grid", "Grid",
                       {{"count", Setting(count)},
                        {"template", Setting(std::string("Cell"))},
                        {"join", Setting(std::string(join))}});
  if (depths) {
    g.values = g.graph.add("list.range", "Depths",
                           {{"start", Setting(depths->first)},
                            {"step", Setting(depths->second)},
                            {"count", Setting(count.x * count.y * count.z)}});
    g.graph.find_node(g.grid)->settings["parameter"] = Setting(std::string("depth"));
    g.graph.link(g.values, "list", g.grid, "values");
  }
  g.delegate = g.graph.add("children.delegate", "Seat",
                           {{"surface", Setting(seat.to_string())}});
  g.output = g.graph.add("output.children", "Children");
  g.graph.link(g.grid, "children", g.delegate, "children");
  g.graph.link(g.delegate, "children", g.output, "children");
  return g;
}

// Boxes of `size` at a pattern.grid of frames from `start`, `spacing` apart.
Graph frames_graph(dvec3 start, dvec3 counts, dvec3 spacing, dvec3 size) {
  Graph g;
  const Id p = g.add("frames.point", "Start", {{"position", Setting(start)}});
  const Id grid = g.add("pattern.grid", "Grid", {{"counts", Setting(counts)},
                                                 {"spacing", Setting(spacing)}});
  const Id boxes = g.add("children.at_frames", "Boxes",
                         {{"size", Setting(size)}, {"template", Setting(std::string("Cell"))}});
  const Id out = g.add("output.children", "Children");
  g.link(p, "frames", grid, "frames");
  g.link(grid, "frames", boxes, "frames");
  g.link(boxes, "children", out, "children");
  return g;
}

rack::Rack rack_with(const Graph& graph) { return rack::make_rack(to_json(graph)); }

std::size_t index_of(const model::Document& doc, const Id& volume) {
  for (std::size_t i = 0; i < doc.volumes.size(); ++i) {
    if (doc.volumes[i].id == volume) return i;
  }
  FAIL("no such volume");
  return 0;
}

// The Rack's generator run directly.
GraphEvaluation run(const rack::Rack& r, const Graph& graph) {
  const auto& doc = r.project.document;
  const auto layout = model::build_layout(doc, kSettings);
  auto inputs = gather_inputs(doc, layout, index_of(doc, r.rack), kSettings);
  REQUIRE(inputs);
  return evaluate_graph(graph, *inputs);
}

std::vector<const eval::VolumeResult*> generated(const eval::Evaluation& ev, const Id& rack) {
  std::vector<const eval::VolumeResult*> out;
  for (std::size_t i = 0; i < ev.document.volumes.size(); ++i) {
    const auto& v = ev.document.volumes[i];
    if (v.generated && v.parent == rack) out.push_back(&ev.volumes[i]);
  }
  return out;
}

bool has(const core::Diagnostics& ds, const std::string& code) {
  return !ds.with_code(code).empty();
}

}  // namespace

TEST_CASE("children.grid tiles the boundary and joins neighbours", "[proc][children]") {
  const rack::Rack r = rack::make_rack();
  const GridGraph g = grid_graph(r.seat, {3, 2, 1}, "bonded", std::pair{4.0, 1.0});
  const auto ev = run(r, g.graph);
  INFO(scenes::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  REQUIRE(ev.children);
  const model::GeneratedLayout& l = **ev.children;
  REQUIRE(l.children.size() == 6);
  double volume = 0;
  for (std::size_t k = 0; k < 6; ++k) {
    const auto& c = l.children[k];
    CHECK(c.template_name == "Cell");
    CHECK(c.name == "Cell " + std::to_string(k + 1));
    CHECK(c.parameters.at("depth") == 4.0 + static_cast<double>(k));
    const dvec3 size = c.box.max - c.box.min;
    CHECK_THAT(size.x, WithinAbs(20, 1e-9));
    CHECK_THAT(size.y, WithinAbs(20, 1e-9));
    volume += size.x * size.y * size.z;
  }
  CHECK_THAT(volume, WithinRel(60.0 * 40 * 10, 1e-12));
  CHECK(l.children[1].box.min.x == 20.0);  // x fastest
  CHECK(l.children[3].box.min.y == 20.0);
  // 2 x 2 joins across x, 3 across y; each inset by the 1 mm margin.
  REQUIRE(l.surfaces.size() == 7);
  for (const auto& s : l.surfaces) {
    // In-plane axes: across x, (y, z); across y, (z, x). Cells are 20 x 20 x 10.
    CHECK(s.hi[0] - s.lo[0] == (s.axis == 0 ? 18.0 : 8.0));
    CHECK(s.hi[1] - s.lo[1] == (s.axis == 0 ? 8.0 : 18.0));
    CHECK(s.type == model::ConnectionType::Bonded);
  }
  REQUIRE(l.delegations.size() == 1);
  CHECK(l.delegations[0].surface == r.seat);
  CHECK(l.delegations[0].child == 0);

  // Keys are stable: the same graph gives the same layout.
  const auto again = run(r, g.graph);
  REQUIRE(again.children);
  CHECK(hash_value(Value(*again.children)) == hash_value(Value(*ev.children)));
}

TEST_CASE("children.at_frames places boxes; values must match the children",
          "[proc][children]") {
  const rack::Rack r = rack::make_rack();
  Graph g = frames_graph({10, 10, 5}, {3, 2, 1}, {20, 20, 10}, {20, 20, 10});
  const auto ev = run(r, g);
  REQUIRE(ev.ok());
  const auto& l = **ev.children;
  REQUIRE(l.children.size() == 6);
  CHECK(l.children[0].box == dbox3::from_min_max({0, 0, 0}, {20, 20, 10}));
  CHECK(l.surfaces.empty());

  // Three values for six children.
  const Id values = g.add("list.range", "Values", {{"count", Setting(3.0)}});
  Node* boxes = g.find_node(g.find_node(std::string_view("Boxes"))->id);
  boxes->settings["parameter"] = Setting(std::string("depth"));
  g.link(values, "list", boxes->id, "values");
  const auto bad = run(r, g);
  CHECK_FALSE(bad.ok());
  CHECK(has(bad.diagnostics, "proc.node.infeasible"));
}

TEST_CASE("children.merge and children.delegate", "[proc][children]") {
  const rack::Rack r = rack::make_rack();
  Graph g;
  const Id left = g.add("children.at_frames", "Left",
                        {{"size", Setting(dvec3{20, 20, 10})}});
  const Id right = g.add("children.at_frames", "Right",
                         {{"size", Setting(dvec3{20, 20, 10})}});
  const Id lp = g.add("frames.point", "L", {{"position", Setting(dvec3{40, 30, 5})}});
  const Id rp = g.add("frames.point", "R", {{"position", Setting(dvec3{10, 10, 5})}});
  const Id merge = g.add("children.merge", "Both");
  const Id seat = g.add("children.delegate", "Seat", {{"surface", Setting(r.seat.to_string())}});
  const Id out = g.add("output.children", "Children");
  g.link(lp, "frames", left, "frames");
  g.link(rp, "frames", right, "frames");
  g.link(left, "children", merge, "a");
  g.link(right, "children", merge, "b");
  g.link(merge, "children", seat, "children");
  g.link(seat, "children", out, "children");
  const auto ev = run(r, g);
  INFO(scenes::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  const auto& l = **ev.children;
  REQUIRE(l.children.size() == 2);
  CHECK(l.children[0].name == "Cell 1");  // placeholders
  CHECK(l.children[0].template_name.empty());
  REQUIRE(l.delegations.size() == 1);
  CHECK(l.delegations[0].child == 1);  // the box over the seat

  // Nothing on the patch: infeasible.
  g.find_node(rp)->settings["position"] = Setting(dvec3{50, 30, 5});
  const auto none = run(r, g);
  CHECK(has(none.diagnostics, "proc.node.infeasible"));
}

TEST_CASE("A composite's generator ends in output.children, and only there",
          "[proc][children][check]") {
  const rack::Rack r = rack::make_rack();
  const auto& doc = r.project.document;
  const GridGraph g = grid_graph(r.seat);
  CHECK_FALSE(check_volume_graph(g.graph, doc, r.rack).has_errors());
  // A procedural volume can't emit children.
  CHECK(has(check_volume_graph(g.graph, doc, r.base), "proc.graph.output_kind"));
  // A composite's graph can't end in contents.
  CHECK(has(check_volume_graph(scenes::shell_and_ball().graph, doc, r.rack),
            "proc.graph.output_kind"));
  // Two outputs, of either kind.
  Graph two = g.graph;
  two.add("output", "Contents");
  CHECK(has(check_graph(two), "proc.graph.output"));
  // A template the composite doesn't have.
  Graph unknown = g.graph;
  unknown.find_node(g.grid)->settings["template"] = Setting(std::string("Bracket"));
  CHECK(has(check_volume_graph(unknown, doc, r.rack), "proc.reference.template"));
  // Children only connect to children.
  Graph wrong = g.graph;
  const Id ball = wrong.add("field.sphere", "Ball");
  wrong.link(ball, "field", g.output, "children");
  CHECK(has(check_graph(wrong), "proc.graph.type_mismatch"));
}

TEST_CASE("A generated grid of parametric cells evaluates, sums up and welds",
          "[proc][children][engine]") {
  rack::Rack rr = rack::make_rack();
  auto& comp =
      std::get<model::CompositeContents>(rr.project.document.find_volume(rr.rack)->contents);
  comp.generator = to_json(grid_graph(rr.seat).graph);

  eval::Engine engine(kSettings);
  install(engine);
  eval::EvaluateOptions o;
  o.weld = true;
  const auto ev = engine.evaluate(rr.project, o);
  for (const auto& d : ev.diagnostics) INFO(d.to_string());
  CHECK(ev.ok());
  CHECK(ev.stats.layouts_generated == 1);

  const auto cells = generated(ev, rr.rack);
  REQUIRE(cells.size() == 6);
  for (const auto* c : cells) CHECK(c->status == model::VolumeStatus::Current);
  const auto* rack_result = ev.find(rr.rack);
  REQUIRE(rack_result);
  CHECK(rack_result->status == model::VolumeStatus::Current);
  REQUIRE(rack_result->contents);
  REQUIRE(rack_result->contents->outputs.size() == 1);
  CHECK_THAT(std::get<double>(*rack_result->contents->outputs[0].value),
             WithinRel(60.0 * 40 * 10, 1e-6));

  REQUIRE(ev.combined);
  const auto& w = ev.combined->weld;
  INFO(scenes::codes(ev.combined->diagnostics));
  CHECK(w.ok());
  CHECK(w.topology.watertight());
  CHECK(w.topology.manifold());

  // The authored project is untouched; the evaluated document holds them.
  CHECK(rr.project.document.volumes.size() == 2);
  CHECK(ev.document.volumes.size() == 8);
  CHECK_FALSE(model::check(ev.document).has_errors());

  // Again: everything from the cache, the generator not run.
  const auto twice = engine.evaluate(rr.project);
  CHECK(twice.stats.layouts_generated == 0);
  CHECK(twice.stats.regenerated == 0);
  CHECK(twice.output_hash() == engine.evaluate(rr.project).output_hash());
}

TEST_CASE("Generated layouts are deterministic and regenerate only what changed",
          "[proc][children][engine]") {
  rack::Rack r = rack::make_rack();
  auto& comp = std::get<model::CompositeContents>(r.project.document.find_volume(r.rack)->contents);
  comp.generator = to_json(grid_graph(r.seat, {3, 2, 1}, "bonded", std::pair{6.0, 0.5}).graph);

  const auto hashes = eval::output_hashes_by_threads(r.project, kSettings, {1, 4}, true);
  REQUIRE(hashes.size() == 2);
  CHECK(hashes[0] == hashes[1]);

  eval::Engine engine(kSettings);
  install(engine);
  const auto first = engine.evaluate(r.project);
  REQUIRE(first.ok());

  // Moving the seat changes only the cell it is passed to (and the Rack).
  auto& seat = std::get<model::DatumPatch>(r.project.document.find_connection(r.seat)->geometry);
  seat.u = feature::Param::of(9);
  const auto moved = engine.evaluate(r.project);
  REQUIRE(moved.ok());
  std::size_t regenerated = 0;
  for (const auto* c : generated(moved, r.rack)) regenerated += c->regenerated ? 1 : 0;
  CHECK(regenerated == 1);
  CHECK(moved.find(r.base)->regenerated);

  // A different count is a different layout: the generator runs again.
  comp.generator = to_json(grid_graph(r.seat, {2, 2, 1}).graph);
  const auto fewer = engine.evaluate(r.project);
  CHECK(fewer.ok());
  CHECK(fewer.stats.layouts_generated == 1);
  CHECK(generated(fewer, r.rack).size() == 4);
}

TEST_CASE("Generated layouts pass the same checks as authored ones",
          "[proc][children][engine]") {
  eval::Engine engine(kSettings);
  install(engine);

  SECTION("overlap between generated siblings") {
    const auto r = rack_with(frames_graph({10, 10, 5}, {2, 1, 1}, {15, 0, 0}, {20, 20, 10}));
    const auto ev = engine.evaluate(r.project);
    CHECK(has(ev.diagnostics, "validate.overlap"));
    CHECK(has(ev.find(r.rack)->diagnostics, "eval.generated.layout"));
    CHECK(ev.find(r.rack)->status == model::VolumeStatus::Failed);
  }
  SECTION("a child reaching outside its composite") {
    const auto r = rack_with(frames_graph({55, 10, 5}, {1, 1, 1}, {0, 0, 0}, {20, 20, 10}));
    const auto ev = engine.evaluate(r.project);
    CHECK(has(ev.diagnostics, "validate.nesting.outside"));
    CHECK(ev.find(r.rack)->status == model::VolumeStatus::Failed);
  }
  SECTION("contents not contained in a generated child") {
    rack::Rack r = rack::make_rack();
    auto& comp =
        std::get<model::CompositeContents>(r.project.document.find_volume(r.rack)->contents);
    // Every cell 12 deep in a 10 mm box: the fill pokes out of the bottom.
    comp.generator = to_json(grid_graph(r.seat, {3, 2, 1}, "bonded", std::pair{12.0, 0.0}).graph);
    const auto ev = engine.evaluate(r.project);
    CHECK(has(ev.diagnostics, "validate.containment"));
    for (const auto* c : generated(ev, r.rack)) CHECK(c->status == model::VolumeStatus::Failed);
    CHECK(ev.find(r.rack)->status == model::VolumeStatus::Failed);
  }
  SECTION("undeclared contact between generated siblings") {
    rack::Rack r = rack::make_rack();
    auto& comp =
        std::get<model::CompositeContents>(r.project.document.find_volume(r.rack)->contents);
    comp.generator = to_json(grid_graph(r.seat, {3, 2, 1}, "none").graph);
    const auto ev = engine.evaluate(r.project);
    CHECK(has(ev.diagnostics, "validate.contact"));
    CHECK(ev.find(r.rack)->status != model::VolumeStatus::Failed);  // a warning
  }
  SECTION("a generator that fails") {
    rack::Rack r = rack::make_rack();
    auto& comp =
        std::get<model::CompositeContents>(r.project.document.find_volume(r.rack)->contents);
    comp.generator = to_json(grid_graph(r.seat, {0, 1, 1}).graph);
    const auto ev = engine.evaluate(r.project);
    CHECK(has(ev.find(r.rack)->diagnostics, "proc.node.infeasible"));
    CHECK(ev.find(r.rack)->status == model::VolumeStatus::Failed);
    CHECK(generated(ev, r.rack).empty());
    CHECK(ev.find(r.base)->status == model::VolumeStatus::Current);
  }
}

TEST_CASE("Without a layout generator a generating composite is stale",
          "[proc][children][engine]") {
  rack::Rack r = rack::make_rack();
  auto& comp = std::get<model::CompositeContents>(r.project.document.find_volume(r.rack)->contents);
  comp.generator = to_json(grid_graph(r.seat).graph);
  eval::Engine engine(kSettings);  // nothing installed
  const auto ev = engine.evaluate(r.project);
  CHECK(ev.find(r.rack)->status == model::VolumeStatus::Stale);
  CHECK(has(ev.find(r.rack)->diagnostics, "eval.generated.no_generator"));
  CHECK(ev.find(r.base)->status == model::VolumeStatus::Current);
}

TEST_CASE("Procedural templates fill generated cells, stitch and weld",
          "[proc][children][engine][mesher]") {
  rack::Rack r = rack::make_rack();
  auto& comp = std::get<model::CompositeContents>(r.project.document.find_volume(r.rack)->contents);
  comp.generator = to_json(grid_graph(r.seat, {3, 2, 1}).graph);
  auto& cell = comp.templates.front();
  cell.contents = model::ProceduralContents{to_json(scenes::shell_and_ball().graph)};
  cell.parameters.front().name = "wall";
  cell.parameters.front().value = feature::Param::of(2);

  eval::Engine engine(kSettings);
  install(engine);
  eval::EvaluateOptions o;
  o.weld = true;
  const auto ev = engine.evaluate(r.project, o);
  for (const auto& d : ev.diagnostics) INFO(d.to_string());
  CHECK(ev.ok());
  CHECK(ev.diagnostics.with_code("proc.mesh.seam_uncovered").empty());
  for (const auto* c : generated(ev, r.rack)) {
    CHECK(c->status == model::VolumeStatus::Current);
    REQUIRE(contents_mesh(*c) != nullptr);
  }
  REQUIRE(ev.combined);
  CHECK(ev.combined->weld.ok());
  CHECK(ev.combined->weld.topology.watertight());
  CHECK(ev.combined->weld.topology.manifold());
}

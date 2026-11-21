// M12: baking, unbaking (acceptance 2: "bake then unbake round-trips the
// volume's content hash"), published outputs and node inspection.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/mesh/io.hpp"
#include "vs/model/check.hpp"
#include "vs/proc/bake.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/inspect.hpp"
#include "vs/proc/templates.hpp"

using namespace vs;
using namespace vs::proc;
using Catch::Approx;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

eval::Engine engine_with(std::size_t threads = 2) {
  eval::EngineOptions o;
  o.threads = threads;
  return eval::Engine(kSettings, o);
}

}  // namespace

TEST_CASE("Acceptance 2: bake then unbake round-trips the volume's content hash",
          "[proc][bake][acceptance]") {
  scenes::Mixed m = scenes::mixed(scenes::shell_and_ball().graph, true);
  const model::Project original = m.project;
  const core::Hash256 before = model::content_hash(original);

  eval::Engine engine = engine_with();
  install(engine);
  eval::EvaluateOptions weld;
  weld.weld = true;
  const auto generated = engine.evaluate(m.project, weld);
  REQUIRE(generated.ok());
  const mesh::Mesh* pm = contents_mesh(*generated.find(m.procedural));
  REQUIRE(pm != nullptr);

  // Bake: a static 3MF volume that keeps the graph.
  core::Diagnostics d = bake(m.project, m.procedural, *pm);
  INFO(scenes::codes(d));
  REQUIRE_FALSE(d.has_errors());
  const model::Volume* baked = m.project.document.find_volume(m.procedural);
  REQUIRE(baked != nullptr);
  CHECK(baked->kind() == model::VolumeKind::Static);
  CHECK(is_baked(*baked));
  const auto& stat = std::get<model::StaticContents>(baked->contents);
  CHECK(stat.format == "3mf");
  REQUIRE(m.project.blobs.contains(stat.blob));
  CHECK(model::content_hash(m.project) != before);
  // The blob holds the generated mesh exactly: the same triangles, corner
  // for corner (3MF numbers vertices in order of use).
  auto read = mesh::read_3mf(*m.project.blobs.at(stat.blob));
  REQUIRE(read);
  REQUIRE(read->triangle_count() == pm->triangle_count());
  bool same = true;
  for (std::uint32_t t = 0; t < pm->triangle_count() && same; ++t) {
    for (std::size_t k = 0; k < 3; ++k) {
      same = same && read->positions[read->triangle(t)[k]] == pm->positions[pm->triangle(t)[k]];
    }
  }
  CHECK(same);

  // The baked volume evaluates as static contents: no generation, still
  // contained, and it still welds to its neighbour across the flange.
  const auto after = engine.evaluate(m.project, weld);
  INFO(scenes::codes(after.diagnostics));
  REQUIRE(after.ok());
  const auto* bv = after.find(m.procedural);
  REQUIRE(bv != nullptr);
  CHECK(bv->status == model::VolumeStatus::Current);
  CHECK(field_contents(*bv) == nullptr);
  REQUIRE(after.combined);
  CHECK(after.combined->weld.ok());
  CHECK(after.combined->weld.topology.watertight());
  CHECK(after.combined->weld.topology.manifold());
  CHECK(after.combined->weld.mesh.triangle_count() == generated.combined->weld.mesh.triangle_count());

  // A project save and load keeps it baked.
  auto manifest = model::project_from_manifest(model::manifest_json(m.project));
  REQUIRE(manifest);
  CHECK(manifest->document == m.project.document);

  // Unbake: the same project as before, byte for byte.
  d = unbake(m.project, m.procedural);
  REQUIRE_FALSE(d.has_errors());
  CHECK(m.project == original);
  CHECK(model::content_hash(m.project) == before);
  CHECK(m.project.blobs.empty());
}

TEST_CASE("Bake and unbake refuse what they can't do", "[proc][bake]") {
  scenes::Mixed m = scenes::mixed(scenes::shell_and_ball().graph);
  const mesh::Mesh empty;
  CHECK_FALSE(bake(m.project, m.procedural, empty).with_code("proc.bake.empty").empty());
  CHECK_FALSE(bake(m.project, m.parametric, empty).with_code("proc.bake.kind").empty());
  CHECK_FALSE(unbake(m.project, m.procedural).with_code("proc.bake.kind").empty());
  CHECK(bake(m.project, core::Id::random(), empty).has_errors());
}

TEST_CASE("Procedural volumes publish measured and graph outputs", "[proc][outputs]") {
  // Gyroid template with a published count of its parameters' worth: the
  // bolt boss template publishes boss_count on a flange.
  scenes::Mixed m = scenes::mixed(scenes::shell_and_ball().graph, true);
  TemplateOptions o;
  o.surface = m.surface;
  o.cell_size = 1.0;
  auto graph = make_template("bolt_bosses", o);
  REQUIRE(graph);
  model::Volume& v = *m.project.document.find_volume(m.procedural);
  v.contents = model::ProceduralContents{to_json(*graph)};
  const core::Id vol = core::Id::random(), cog = core::Id::random(), env = core::Id::random(),
                 count = core::Id::random(), missing = core::Id::random();
  v.outputs = {{vol, "volume", model::OutputQuantity::Volume, 1},
               {cog, "cog", model::OutputQuantity::CentreOfGravity, 1},
               {env, "envelope", model::OutputQuantity::Envelope, 1},
               {count, "boss_count", model::OutputQuantity::Published, 1},
               {missing, "nothing", model::OutputQuantity::Published, 1}};
  eval::Engine engine = engine_with();
  install(engine);
  const auto ev = engine.evaluate(m.project);
  INFO(scenes::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  const auto& outs = ev.find(m.procedural)->contents->outputs;
  REQUIRE(outs.size() == 5);
  const mesh::Mesh& mesh = *contents_mesh(*ev.find(m.procedural));
  CHECK(std::get<double>(*outs[0].value) == Approx(mesh::enclosed_volume(mesh)));
  const auto c = std::get<core::dvec3>(*outs[1].value);
  CHECK(c.x == Approx(20.0).margin(2.0));  // roughly the box's middle, pulled toward the boss
  CHECK(c.x > 20.0);
  const auto box = std::get<core::dbox3>(*outs[2].value);
  CHECK(box.max.x == Approx(40.0).margin(0.02));
  CHECK(std::get<double>(*outs[3].value) == 1.0);
  CHECK_FALSE(outs[4].value.has_value());
  CHECK_FALSE(ev.diagnostics.with_code("proc.output.unpublished").empty());
}

TEST_CASE("Any node's outputs can be inspected without the rest of the graph", "[proc][inspect]") {
  scenes::Boxes b = scenes::boxes({scenes::shell_and_ball().graph});
  const model::DocumentBuild layout = model::build_layout(b.project.document, kSettings);
  auto inputs = gather_inputs(b.project.document, layout, 0, kSettings);
  REQUIRE(inputs);

  // An unfinished graph: no output node, a dangling node elsewhere.
  Graph g;
  const Id boundary = g.add("input.boundary", "Boundary");
  const Id gyroid = g.add("field.tpms", "Gyroid");
  const Id clip = g.add("field.intersection", "Clip");
  g.link(gyroid, "field", clip, "a");
  g.link(boundary, "field", clip, "b");
  g.add("field.union", "Unfinished");
  const Id row = g.add("pattern.grid", "Grid");

  NodeCache cache;
  EvaluateGraphOptions o;
  o.cache = &cache;
  const NodeInspection ins = inspect_node(g, *inputs, clip, o);
  INFO(scenes::codes(ins.diagnostics));
  REQUIRE(ins.ok());
  CHECK(ins.evaluated == 3);  // boundary, gyroid, clip: not the grid
  REQUIRE(ins.outputs.size() == 1);
  CHECK(ins.outputs[0].first == "field");
  // Again: from the cache (the boundary input always runs).
  const NodeInspection again = inspect_node(g, *inputs, clip, o);
  CHECK(again.from_cache);
  CHECK(again.evaluated == 1);
  CHECK(hash_value(again.outputs[0].second) == hash_value(ins.outputs[0].second));

  // Previews: a field becomes a mesh of its region, frames become points.
  const Preview pf = preview(ins.outputs[0].second, *inputs);
  CHECK(pf.type == PortType::Field);
  REQUIRE(pf.mesh);
  CHECK(pf.mesh->triangle_count() > 100);
  REQUIRE(pf.samples);
  CHECK(pf.samples->grid.cell == Approx(40.0 / kPreviewCells));
  const NodeInspection grid = inspect_node(g, *inputs, row);
  REQUIRE(grid.ok());
  const Preview pg = preview(*grid.find("frames"), *inputs);
  CHECK(pg.points.size() == 9);
  const Preview ps = preview(*grid.find("count"), *inputs);
  CHECK(ps.text == "9");
  CHECK_FALSE(ps.mesh);

  // A node whose own inputs are missing can't be inspected.
  const Id lonely = g.add("field.offset", "Lonely");
  const NodeInspection bad = inspect_node(g, *inputs, lonely);
  CHECK_FALSE(bad.ok());
  CHECK_FALSE(bad.diagnostics.with_code("proc.graph.missing_input").empty());

  // Through the volume: its own graph's nodes.
  const auto& contents = std::get<model::ProceduralContents>(b.project.document.volumes[0].contents);
  const auto own = graph_from_json(contents.graph);
  REQUIRE(own);
  const Node* shell = own->find_node("Shell");
  REQUIRE(shell != nullptr);
  const NodeInspection through =
      inspect_volume_node(b.project.document, layout, 0, kSettings, shell->id);
  REQUIRE(through.ok());
  CHECK(through.evaluated == 3);  // boundary, wall parameter, shell
}

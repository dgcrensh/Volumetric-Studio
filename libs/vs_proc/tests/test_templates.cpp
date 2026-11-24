// M12's acceptance item 1 (Implementation Plan, M12): each starter
// template generates, passes containment, welds and exports to 3MF.

#include <string>

#include <fmt/format.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/mesh/io.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/proc/check.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/templates.hpp"

using namespace vs;
using namespace vs::proc;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

core::Hash256 mesh_hash(const mesh::Mesh& m) {
  core::Hasher h;
  for (const auto& p : m.positions) h.add(p);
  for (const auto& id : m.seam_vertices) h.add(id);
  for (const auto i : m.indices) h.add(i);
  for (const auto& id : m.triangle_volumes) h.add(id);
  return h.finalize();
}

// The template filling a mixed scene's procedural box (a bolted flange for
// bolt_bosses, bonded otherwise), at a 0.5 mm grid.
scenes::Mixed scene_for(const std::string& name) {
  const bool flange = name == "bolt_bosses";
  // The surface id is only known once the scene exists: build it, then
  // put the template in.
  scenes::Mixed m = scenes::mixed(scenes::shell_and_ball().graph, flange);
  TemplateOptions o;
  o.surface = m.surface;
  o.cell_size = 0.5;
  auto graph = make_template(name, o);
  REQUIRE(graph);
  m.project.document.find_volume(m.procedural)->contents = model::ProceduralContents{to_json(*graph)};
  model::PublishedOutput count{core::Id::random(), "boss_count", model::OutputQuantity::Published, 1};
  model::PublishedOutput mass{core::Id::random(), "mass", model::OutputQuantity::Mass, 1.24};
  auto& outputs = m.project.document.find_volume(m.procedural)->outputs;
  outputs.push_back(mass);
  if (flange) outputs.push_back(count);
  return m;
}

}  // namespace

TEST_CASE("The template library lists four templates, each a valid graph", "[proc][templates]") {
  REQUIRE(templates().size() == 4);
  for (const auto& t : templates()) {
    INFO(t.name);
    TemplateOptions o;
    o.surface = core::Id::random();
    auto g = make_template(t.name, o);
    REQUIRE(g);
    CHECK_FALSE(check_graph(*g).has_errors());
    // Same options, same graph, byte for byte (ids derive from the name).
    CHECK(to_json(*make_template(t.name, o)) == to_json(*g));
    // JSON round trip.
    auto back = graph_from_json(to_json(*g));
    REQUIRE(back);
    CHECK(*back == *g);
  }
  CHECK(make_template("teapot").error().code == "proc.template.unknown");
  CHECK(make_template("bolt_bosses").error().code == "proc.template.surface");
  CHECK(make_template("voronoi_walls").error().code == "proc.template.surface");
}

TEST_CASE("Acceptance 1: each template generates, passes containment, welds and exports to 3MF",
          "[proc][templates][acceptance]") {
  const std::string name = GENERATE("gyroid_infill", "graded_lattice", "bolt_bosses", "voronoi_walls");
  INFO(name);
  const scenes::Mixed m = scene_for(name);
  eval::EngineOptions eo;
  eo.threads = 4;
  eval::Engine engine(kSettings, eo);
  install(engine);
  eval::EvaluateOptions o;
  o.weld = true;
  const auto ev = engine.evaluate(m.project, o);
  INFO(scenes::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  CHECK(ev.diagnostics.with_code("validate.containment").empty());

  const auto* p = ev.find(m.procedural);
  REQUIRE(p != nullptr);
  CHECK(p->status == model::VolumeStatus::Current);
  const mesh::Mesh* pm = contents_mesh(*p);
  REQUIRE(pm != nullptr);
  CHECK(pm->triangle_count() > 1000);
  const FieldContents* fc = field_contents(*p);
  REQUIRE(fc != nullptr);
  REQUIRE(fc->stitched.size() == 1);  // the seam is stitched, so it welds

  // Outputs: mass from the mesh; bolt_bosses publishes its boss count.
  REQUIRE(p->contents);
  const auto& outs = p->contents->outputs;
  REQUIRE_FALSE(outs.empty());
  REQUIRE(outs[0].value);
  CHECK(std::get<double>(*outs[0].value) ==
        Catch::Approx(mesh::enclosed_volume(*pm) * 1e-3 * 1.24).epsilon(1e-9));
  if (name == "bolt_bosses") {
    REQUIRE(outs.size() == 2);
    REQUIRE(outs[1].value);
    CHECK(std::get<double>(*outs[1].value) == 1.0);
  }

  REQUIRE(ev.combined);
  const auto& w = ev.combined->weld;
  CHECK(w.ok());
  CHECK(w.topology.watertight());
  CHECK(w.topology.manifold());
  CHECK(w.internal_triangles > 0);
  CHECK(w.mesh.volumes().size() == 2);

  // 3MF: one object per volume, read back unchanged.
  std::vector<mesh::ThreeMfObject> objects;
  std::vector<mesh::Mesh> parts;
  for (const auto& id : w.mesh.volumes()) parts.push_back(mesh::extract_volume(w.mesh, id));
  for (std::size_t i = 0; i < parts.size(); ++i) {
    objects.push_back({fmt::format("part {}", i), w.mesh.volumes()[i], &parts[i]});
  }
  auto file = mesh::write_3mf(objects, name);
  REQUIRE(file);
  auto back = mesh::read_3mf(*file);
  REQUIRE(back);
  CHECK(back->triangle_count() == w.mesh.triangle_count());
  const auto topology = mesh::analyse(*back);
  CHECK(topology.manifold());
}

TEST_CASE("Template meshes are identical across thread counts", "[proc][templates]") {
  const std::string name = GENERATE("gyroid_infill", "graded_lattice", "bolt_bosses", "voronoi_walls");
  INFO(name);
  const scenes::Mixed m = scene_for(name);
  core::Hash256 first{};
  for (const std::size_t n : {1u, 3u}) {
    eval::EngineOptions eo;
    eo.threads = n;
    eval::Engine engine(kSettings, eo);
    install(engine);
    const auto ev = engine.evaluate(m.project);
    REQUIRE(ev.ok());
    const mesh::Mesh* pm = contents_mesh(*ev.find(m.procedural));
    REQUIRE(pm != nullptr);
    if (n == 1) {
      first = mesh_hash(*pm);
    } else {
      CHECK(mesh_hash(*pm) == first);
    }
  }
}

TEST_CASE("A cancelled Voronoi evaluation keeps nothing", "[proc][templates][voronoi][cancel]") {
  const scenes::Mixed m = scene_for("voronoi_walls");
  eval::EngineOptions eo;
  eo.threads = 2;
  eval::Engine engine(kSettings, eo);
  install(engine);
  eval::CancellationToken token;
  token.cancel();
  eval::EvaluateOptions o;
  o.cancel = token;
  const auto ev = engine.evaluate(m.project, o);
  CHECK(ev.cancelled);
  const auto* p = ev.find(m.procedural);
  REQUIRE(p != nullptr);
  CHECK_FALSE(p->contents);
  CHECK(engine.cache().memory_size() == 0);
  // The next evaluation generates it whole.
  const auto again = engine.evaluate(m.project);
  INFO(scenes::codes(again.diagnostics));
  REQUIRE(again.ok());
  CHECK(again.find(m.procedural)->regenerated);
}

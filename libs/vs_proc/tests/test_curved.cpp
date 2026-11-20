// L2: procedural contents stitched to curved seams, welded through the
// engine to parametric neighbours, and contained.

#include <numbers>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "curved.hpp"
#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/proc/generator.hpp"

using namespace vs;
using namespace vs::proc;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

// One volume procedural (the shell-and-ball graph: its boundary shelled 2 mm
// deep, so it covers every face), the other filled parametrically.
curved::Curved scene(bool half, bool procedural_shaft) {
  curved::Options o;
  o.half = half;
  auto c = curved::make_curved(o);
  auto& doc = c.project.document;
  const Id proc_id = procedural_shaft ? c.shaft : c.sleeve;
  const Id param_id = procedural_shaft ? c.sleeve : c.shaft;
  model::Volume* p = doc.find_volume(proc_id);
  p->parameters.push_back({Id::random(), "wall", feature::Param::of(2)});
  p->seed = 3;
  p->contents = model::ProceduralContents{to_json(scenes::shell_and_ball().graph)};
  doc.find_volume(param_id)->contents = curved::fill(o, !procedural_shaft);
  return c;
}

}  // namespace

TEST_CASE("Procedural contents weld to parametric ones across curved surfaces",
          "[proc][mesher][curved]") {
  const bool half = GENERATE(false, true);
  const bool procedural_shaft = GENERATE(false, true);
  INFO((half ? "half shell" : "band") << ", procedural " << (procedural_shaft ? "shaft" : "sleeve"));
  const auto c = scene(half, procedural_shaft);
  eval::Engine engine(kSettings);
  install(engine);
  eval::EvaluateOptions o;
  o.weld = true;
  const auto ev = engine.evaluate(c.project, o);
  INFO(scenes::codes(ev.diagnostics));
  for (const auto& d : ev.diagnostics) INFO(d.to_string());
  CHECK(ev.ok());
  CHECK(ev.diagnostics.with_code("validate.containment").empty());
  CHECK(ev.diagnostics.with_code("proc.mesh.seam_uncovered").empty());
  const std::size_t pi = procedural_shaft ? 0 : 1;
  const FieldContents* fc = field_contents(ev.volumes[pi]);
  REQUIRE(fc != nullptr);
  CHECK(fc->stitched == std::vector<Id>{c.surface});
  const mesh::Mesh* m = contents_mesh(ev.volumes[pi]);
  REQUIRE(m != nullptr);
  const mesh::TopologyReport t = mesh::analyse(*m);
  CHECK(t.watertight());
  CHECK(t.manifold());
  REQUIRE(ev.combined);
  const auto& w = ev.combined->weld;
  CHECK(w.ok());
  CHECK(w.topology.watertight());
  CHECK(w.topology.manifold());
  CHECK(w.internal_triangles > 0);
}

TEST_CASE("Curved stitching is deterministic across thread counts", "[proc][mesher][curved]") {
  const auto c = scene(false, false);
  std::optional<core::Hash256> first;
  for (const std::size_t threads : {1u, 4u}) {
    eval::EngineOptions eo;
    eo.threads = threads;
    eval::Engine engine(kSettings, eo);
    install(engine);
    const auto ev = engine.evaluate(c.project, {});
    REQUIRE(ev.ok());
    const core::Hash256 h = ev.volumes[1].contents->output_hash;
    if (first) CHECK(h == *first);
    first = h;
  }
}

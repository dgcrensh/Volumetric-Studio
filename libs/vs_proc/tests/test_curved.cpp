// L2: procedural contents stitched to curved seams, welded through the
// engine to parametric neighbours, and contained.

#include <cmath>
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
#include "vs/proc/evaluate.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/inputs.hpp"

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

TEST_CASE("Nodes read a curved surface's normal radially, not along its axis (H3)",
          "[proc][curved]") {
  const bool half = GENERATE(false, true);
  INFO((half ? "half shell" : "band"));
  curved::Options o;
  o.half = half;
  const auto c = curved::make_curved(o);
  const auto& doc = c.project.document;
  const auto layout = model::build_layout(doc, kSettings);
  std::size_t shaft = 0;
  while (doc.volumes[shaft].id != c.shaft) ++shaft;
  const auto inputs = gather_inputs(doc, layout, shaft, kSettings);
  REQUIRE(inputs);
  const ConnectionInput* surface = inputs->find_connection(c.surface);
  REQUIRE(surface != nullptr);
  REQUIRE(surface->radius);
  CHECK(*surface->radius == o.radius);

  Graph g;
  const Id points = g.add("scatter.surface", "Points",
                          {{"surface", Setting(c.surface.to_string())}, {"count", Setting(40.0)}});
  const Id joint = g.add("input.surface", "Joint", {{"surface", Setting(c.surface.to_string())}});
  EvaluateGraphOptions eo;
  eo.keep_outputs = true;

  eo.only_for = points;
  const auto scattered = evaluate_graph(g, *inputs, eo);
  INFO(scenes::codes(scattered.diagnostics));
  REQUIRE_FALSE(scattered.diagnostics.has_errors());
  const auto& frames = std::get<Frames>(scattered.find(points)->outputs[0])->frames;
  REQUIRE(frames.size() == 40);
  for (const auto& f : frames) {
    const core::dvec3 p = core::to_fp64(f.translation);
    const core::dvec3 z = core::to_fp64(f.linear.cols[2]);
    const core::dvec3 radial = core::normalize(core::dvec3{p.x, p.y, 0.0});  // the axis is z
    CHECK_THAT(core::length(z), Catch::Matchers::WithinAbs(1.0, 1e-6));
    CHECK_THAT(core::dot(z, radial), Catch::Matchers::WithinAbs(1.0, 1e-6));
  }

  eo.only_for = joint;
  const auto read = evaluate_graph(g, *inputs, eo);
  REQUIRE_FALSE(read.diagnostics.has_errors());
  const Field plane = std::get<Field>(read.find(joint)->outputs[0]);
  // Signed distance to the cylinder of radius 10 round z, wherever along z.
  CHECK_THAT(plane->eval({15, 0, 5}), Catch::Matchers::WithinAbs(5.0, 1e-9));
  CHECK_THAT(plane->eval({0, 4, 25}), Catch::Matchers::WithinAbs(-6.0, 1e-9));
  CHECK_THAT(plane->eval({-900, 900, -900}),
             Catch::Matchers::WithinAbs(std::sqrt(2.0) * 900 - 10.0, 1e-9));
}

TEST_CASE("Offset moves points on a curved surface into either side's volume",
          "[proc][curved][frames][offset]") {
  // The band joins a shaft of radius 10 to a sleeve whose bore is 10: its
  // normal points out of the shaft, so into the shaft is towards the axis
  // and into the sleeve away from it.
  const auto c = curved::make_curved({});
  const auto& doc = c.project.document;
  const auto layout = model::build_layout(doc, kSettings);
  const bool into_shaft = GENERATE(true, false);
  INFO((into_shaft ? "shaft" : "sleeve"));
  std::size_t v = 0;
  while (doc.volumes[v].id != (into_shaft ? c.shaft : c.sleeve)) ++v;
  const auto inputs = gather_inputs(doc, layout, v, kSettings);
  REQUIRE(inputs);

  Graph g;
  const Id points = g.add("scatter.surface", "Points",
                          {{"surface", Setting(c.surface.to_string())}, {"count", Setting(30.0)}});
  const Id moved = g.add("frames.offset", "Inward",
                         {{"distance", Setting(2.0)}, {"layers", Setting(2.0)}, {"step", Setting(3.0)}});
  g.link(points, "frames", moved, "frames");
  EvaluateGraphOptions eo;
  eo.keep_outputs = true;
  eo.only_for = moved;
  const auto result = evaluate_graph(g, *inputs, eo);
  INFO(scenes::codes(result.diagnostics));
  REQUIRE_FALSE(result.diagnostics.has_errors());
  const auto& frames = std::get<Frames>(result.find(moved)->outputs[0])->frames;
  REQUIRE(frames.size() == 60);
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const core::dvec3 p = core::to_fp64(frames[i].translation);
    const double r = std::hypot(p.x, p.y);
    const double depth = i % 2 == 0 ? 2.0 : 5.0;
    CHECK_THAT(r, Catch::Matchers::WithinAbs(into_shaft ? 10.0 - depth : 10.0 + depth, 0.05));  // chords of the tessellated band
    CHECK(inputs->boundary->signed_distance(p) < 0.0);
  }
}

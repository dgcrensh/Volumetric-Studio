// M10's acceptance items (Implementation Plan, M10):
//   1. A graph "boundary shelled by 2 mm, unioned with a sphere" evaluates to
//      a sampled field matching analytic values.
//   2. Type errors and graph loops are rejected at authoring time (also
//      test_check.cpp).
//   3. Graph evaluation is identical across thread counts and runs.

#include <cmath>
#include <random>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/proc/check.hpp"
#include "vs/proc/generator.hpp"

using namespace vs;
using namespace vs::proc;
using core::dvec3;
using Catch::Matchers::WithinAbs;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

eval::EngineOptions threads(std::size_t n) {
  eval::EngineOptions o;
  o.threads = n;
  return o;
}

}  // namespace

TEST_CASE("Acceptance 1: a shelled boundary unioned with a sphere matches analytic values",
          "[proc][acceptance]") {
  const auto s = scenes::shell_and_ball();
  auto b = scenes::boxes({s.graph});
  eval::Engine engine(kSettings, threads(4));
  install(engine);
  const auto ev = engine.evaluate(b.project);
  INFO(scenes::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  const auto& r = ev.volumes.at(0);
  CHECK(r.status == model::VolumeStatus::Current);
  const FieldContents* fc = field_contents(r);
  REQUIRE(fc != nullptr);

  // The grid covers the boundary's box, 1 mm apart.
  const SampledField& sf = fc->samples;
  CHECK(sf.grid.origin == dvec3{0, 0, 0});
  CHECK(sf.grid.cell == 1.0);
  CHECK(sf.grid.nx == 41);
  CHECK(sf.grid.ny == 31);
  CHECK(sf.grid.nz == 21);
  REQUIRE(sf.values.size() == 41u * 31u * 21u);

  const dvec3 lo{0, 0, 0}, hi{40, 30, 20};
  std::size_t inside_shell = 0, inside_ball = 0;
  for (std::uint32_t k = 0; k < sf.grid.nz; ++k) {
    for (std::uint32_t j = 0; j < sf.grid.ny; ++j) {
      for (std::uint32_t i = 0; i < sf.grid.nx; ++i) {
        const dvec3 p = sf.grid.point(i, j, k);
        const double expected = scenes::shell_and_ball_value(p, lo, hi, 2.0);
        REQUIRE_THAT(sf.at(i, j, k), WithinAbs(expected, 1e-5));
        if (sf.at(i, j, k) < 0) {
          if (scenes::box_distance(p, lo, hi) > -2.0) ++inside_shell;
          else ++inside_ball;
        }
      }
    }
  }
  CHECK(inside_shell > 0);
  CHECK(inside_ball > 0);

  // The field itself, lazily, anywhere (outside the box too).
  std::mt19937_64 rng(3);
  std::uniform_real_distribution<double> u(-10.0, 50.0);
  for (int n = 0; n < 500; ++n) {
    const dvec3 p{u(rng), u(rng), u(rng)};
    REQUIRE_THAT(fc->field->eval(p), WithinAbs(scenes::shell_and_ball_value(p, lo, hi, 2.0), 1e-9));
  }

  // The wall follows the volume's parameter.
  auto thicker = b;
  for (auto& p : thicker.project.document.volumes[0].parameters) {
    p.value = feature::Param::of(3.5);
  }
  const auto ev2 = engine.evaluate(thicker.project);
  REQUIRE(ev2.ok());
  const FieldContents* fc2 = field_contents(ev2.volumes[0]);
  REQUIRE(fc2 != nullptr);
  const dvec3 p{2.5, 15, 10};  // 2.5 mm in from the -x face
  CHECK(fc->field->eval(p) > 0.0);
  CHECK(fc2->field->eval(p) < 0.0);
  CHECK_THAT(fc2->field->eval(p), WithinAbs(scenes::shell_and_ball_value(p, lo, hi, 3.5), 1e-9));
}

TEST_CASE("Acceptance 2: type errors and loops are rejected at authoring time",
          "[proc][acceptance]") {
  auto s = scenes::shell_and_ball();
  const Graph before = s.graph;
  // A field into a scalar: the shell's thickness.
  auto d = edit(s.graph, [&](Graph& g) {
    std::erase_if(g.edges, [&](const Edge& e) { return e.to.node == s.shell && e.to.port == "thickness"; });
    g.link(s.ball, "field", s.shell, "thickness");
  });
  CHECK_FALSE(d.with_code("proc.graph.type_mismatch").empty());
  CHECK(s.graph == before);
  // The union feeding the shell, which feeds the union.
  d = edit(s.graph, [&](Graph& g) {
    std::erase_if(g.edges, [&](const Edge& e) { return e.to.node == s.shell && e.to.port == "field"; });
    g.link(s.join, "field", s.shell, "field");
  });
  CHECK_FALSE(d.with_code("proc.graph.loop").empty());
  CHECK(s.graph == before);
}

TEST_CASE("Acceptance 3: evaluation is identical across thread counts and runs",
          "[proc][acceptance]") {
  // Several procedural volumes, one reading a connection surface and a
  // seeded random number, so scheduling and sampling both run in parallel.
  auto s = scenes::shell_and_ball();
  Graph seeded = s.graph;
  {
    const Id rnd = seeded.add("math.random", "Dice", {{"min", Setting(4.0)}, {"max", Setting(8.0)}});
    seeded.link(rnd, "value", s.ball, "radius");
  }
  auto b = scenes::boxes({s.graph, seeded, seeded, s.graph});
  {
    // P1 also cuts a slot along its joint with P2.
    Graph g = seeded;
    const Id dist = g.add("query.distance_to_surface", "Near joint",
                          {{"surface", Setting(b.surfaces[1].to_string())}});
    const Id slot = g.add("field.offset", "Slot", {{"distance", Setting(1.5)}});
    const Id cut = g.add("field.difference", "Cut");
    g.link(dist, "distance", slot, "field");
    for (auto& e : g.edges) {
      if (e.to.node == s.output) e.to = {cut, "a"};
    }
    g.link(slot, "field", cut, "b");
    g.link(cut, "field", s.output, "contents");
    REQUIRE(check_volume_graph(g, b.project.document, b.volumes[1]).empty());
    std::get<model::ProceduralContents>(b.project.document.volumes[1].contents).graph = to_json(g);
  }

  std::vector<core::Hash256> hashes;
  for (const std::size_t n : {1u, 4u, 16u, 4u}) {
    eval::Engine engine(kSettings, threads(n));
    install(engine);
    const auto ev = engine.evaluate(b.project);
    INFO(scenes::codes(ev.diagnostics));
    REQUIRE(ev.ok());
    for (const auto& r : ev.volumes) {
      CHECK(r.status == model::VolumeStatus::Current);
      CHECK(field_contents(r) != nullptr);
    }
    hashes.push_back(ev.output_hash());
  }
  CHECK(hashes[0] == hashes[1]);
  CHECK(hashes[0] == hashes[2]);
  CHECK(hashes[0] == hashes[3]);

  // The engine's own check: generate each volume twice and compare.
  eval::EngineOptions o = threads(4);
  o.check_determinism = true;
  eval::Engine engine(kSettings, o);
  install(engine);
  const auto ev = engine.evaluate(b.project);
  CHECK(ev.diagnostics.with_code("eval.nondeterministic").empty());
  CHECK(ev.output_hash() == hashes[0]);
}

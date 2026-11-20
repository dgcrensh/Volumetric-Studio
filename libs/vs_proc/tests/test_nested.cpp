// L3: a procedural leaf two levels down meets surfaces passed down to it
// from the root and from its composite, stitches to all of them and welds.

#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "nested.hpp"
#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/proc/check.hpp"
#include "vs/proc/generator.hpp"

using namespace vs;
using namespace vs::proc;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

nested::Nested scene() {
  auto n = nested::make_nested();
  nested::fill_leaves(n);
  model::Volume* core = n.project.document.find_volume(n.core);
  core->parameters.push_back({Id::random(), "wall", feature::Param::of(2)});
  core->seed = 5;
  core->contents = model::ProceduralContents{to_json(scenes::shell_and_ball().graph)};
  return n;
}

}  // namespace

TEST_CASE("A procedural grandchild stitches to delegated surfaces and welds",
          "[proc][mesher][nested]") {
  const auto n = scene();
  eval::Engine engine(kSettings);
  install(engine);
  eval::EvaluateOptions o;
  o.weld = true;
  const auto ev = engine.evaluate(n.project, o);
  INFO(scenes::codes(ev.diagnostics));
  for (const auto& d : ev.diagnostics) INFO(d.to_string());
  CHECK(ev.ok());
  CHECK(ev.diagnostics.with_code("validate.containment").empty());
  CHECK(ev.diagnostics.with_code("proc.mesh.seam_uncovered").empty());

  const auto* core = ev.find(n.core);
  REQUIRE(core);
  const FieldContents* fc = field_contents(*core);
  REQUIRE(fc != nullptr);
  CHECK(std::set<Id>(fc->stitched.begin(), fc->stitched.end()) ==
        std::set<Id>{n.seat, n.middle, n.joint});
  REQUIRE(ev.combined);
  const auto& w = ev.combined->weld;
  CHECK(w.ok());
  CHECK(w.topology.watertight());
  CHECK(w.topology.manifold());
}

TEST_CASE("A graph may read a surface passed down to its volume, and no other",
          "[proc][check][nested]") {
  const auto n = scene();
  const auto& doc = n.project.document;
  // Inputs: what the generator sees of Core and of Cap.
  const auto layout = model::build_layout(doc, kSettings);
  REQUIRE(layout.ok());
  std::size_t core = 0;
  std::size_t cap = 0;
  for (std::size_t i = 0; i < doc.volumes.size(); ++i) {
    if (doc.volumes[i].id == n.core) core = i;
    if (doc.volumes[i].id == n.cap) cap = i;
  }
  const auto core_inputs = gather_inputs(doc, layout, core, kSettings);
  REQUIRE(core_inputs);
  CHECK(core_inputs->connections.size() == 3);
  const auto cap_inputs = gather_inputs(doc, layout, cap, kSettings);
  REQUIRE(cap_inputs);
  CHECK(cap_inputs->connections.size() == 1);
}

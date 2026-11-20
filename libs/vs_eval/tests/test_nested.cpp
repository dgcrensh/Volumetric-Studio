// L3: the engine on a nested document. Composites wait for their children,
// add their outputs up and never generate; only what changed regenerates.

#include <map>
#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "nested.hpp"
#include "vs/eval/engine.hpp"
#include "vs/eval/step_export.hpp"

using namespace vs;
using Catch::Matchers::WithinRel;
using core::Id;
using eval::Engine;
using eval::EngineOptions;
using eval::EvaluateOptions;
using model::VolumeStatus;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

std::string describe(const core::Diagnostics& ds) {
  std::string s;
  for (const auto& d : ds) s += d.code + ": " + d.message + "\n";
  return s;
}

std::set<Id> regenerated(const eval::Evaluation& ev) {
  std::set<Id> out;
  for (const auto& v : ev.volumes) {
    if (v.regenerated) out.insert(v.volume);
  }
  return out;
}

std::map<Id, core::Hash256> keys(const eval::Evaluation& ev) {
  std::map<Id, core::Hash256> out;
  for (const auto& v : ev.volumes) {
    if (v.key) out.emplace(v.volume, *v.key);
  }
  return out;
}

nested::Nested filled() {
  auto n = nested::make_nested();
  nested::fill_leaves(n);
  auto& housing = *n.project.document.find_volume(n.housing);
  housing.outputs.push_back({Id::random(), "volume", model::OutputQuantity::Volume, 1.0});
  housing.outputs.push_back(
      {Id::random(), "centre", model::OutputQuantity::CentreOfGravity, 1.0});
  return n;
}

EngineOptions threads(std::size_t n) {
  EngineOptions o;
  o.threads = n;
  return o;
}

}  // namespace

TEST_CASE("A nested document evaluates and welds", "[eval][nested]") {
  auto n = filled();
  Engine engine(kSettings, threads(4));
  EvaluateOptions options;
  options.weld = true;
  const auto ev = engine.evaluate(n.project, options);
  INFO(describe(ev.diagnostics));
  REQUIRE(ev.ok());
  CHECK(regenerated(ev) == std::set<Id>{n.base, n.core, n.cap, n.right});
  for (const auto& v : ev.volumes) CHECK(v.status == VolumeStatus::Current);
  const auto* housing = ev.find(n.housing);
  REQUIRE(housing->contents);
  REQUIRE(housing->key);
  CHECK_THAT(std::get<double>(*housing->contents->outputs[0].value),
             WithinRel(40.0 * 20 * 20, 1e-9));
  const auto centre = std::get<core::dvec3>(*housing->contents->outputs[1].value);
  CHECK_THAT(centre.x, WithinRel(20.0, 1e-9));
  CHECK(ev.build.find_volume(n.housing)->contents_rebuild.has_value());
  REQUIRE(ev.combined);
  CHECK(ev.combined->ok());
  CHECK(ev.combined->weld.topology.watertight());

  // Part names in STEP are paths.
  const auto step = eval::step_parts(ev, {});
  INFO(describe(step.diagnostics));
  REQUIRE(step.parts.size() == 4);
  std::set<std::string> names;
  for (const auto& p : step.parts) names.insert(p.name);
  CHECK(names.contains("Housing / Left / Core"));
  CHECK(names.contains("Base"));
}

TEST_CASE("Editing a grandchild regenerates only it", "[eval][nested]") {
  auto n = filled();
  Engine engine(kSettings, threads(4));
  const auto first = engine.evaluate(n.project);
  REQUIRE(first.ok());

  // Cap's contents shrink inside its boundary.
  n.project.document.find_volume(n.cap)->contents =
      nested::fill(core::dbox3::from_min_max({1, 1, 10}, {19, 19, 20}));
  const auto second = engine.evaluate(n.project);
  INFO(describe(second.diagnostics));
  REQUIRE(second.ok());
  CHECK(regenerated(second) == std::set<Id>{n.cap});
  const auto a = keys(first);
  const auto b = keys(second);
  for (const Id& same : {n.base, n.core, n.right}) CHECK(a.at(same) == b.at(same));
  for (const Id& changed : {n.cap, n.left, n.housing}) CHECK_FALSE(a.at(changed) == b.at(changed));
  // Housing's volume dropped by what Cap lost.
  CHECK_THAT(std::get<double>(*second.find(n.housing)->contents->outputs[0].value),
             WithinRel(16000.0 - (400.0 - 324.0) * 10, 1e-9));

  // Unchanged again: nothing regenerates.
  const auto third = engine.evaluate(n.project);
  CHECK(regenerated(third).empty());
  CHECK(third.output_hash() == second.output_hash());
}

TEST_CASE("Swapping a child changes only its own and its parents' keys", "[eval][nested]") {
  auto n = filled();
  Engine engine(kSettings, threads(2));
  const auto first = engine.evaluate(n.project);
  REQUIRE(first.ok());
  n.project.document.find_volume(n.core)->contents = model::StaticContents{};  // a placeholder
  const auto second = engine.evaluate(n.project);
  INFO(describe(second.diagnostics));
  REQUIRE(second.ok());
  const auto a = keys(first);
  const auto b = keys(second);
  for (const Id& same : {n.base, n.cap, n.right}) CHECK(a.at(same) == b.at(same));
  for (const Id& changed : {n.core, n.left, n.housing}) CHECK_FALSE(a.at(changed) == b.at(changed));
}

TEST_CASE("A link inside a composite runs in waves", "[eval][nested]") {
  auto n = filled();
  auto& doc = n.project.document;
  const Id out = Id::random();
  doc.find_volume(n.core)->outputs.push_back({out, "volume", model::OutputQuantity::Volume, 1.0});
  const Id p = Id::random();
  doc.find_volume(n.cap)->parameters.push_back({p, "p", feature::Param::of(1)});
  doc.links.push_back({Id::random(), {n.core, out}, p});
  Engine engine(kSettings, threads(4));
  const auto ev = engine.evaluate(n.project);
  INFO(describe(ev.diagnostics));
  REQUIRE(ev.ok());
  CHECK(ev.stats.waves >= 2);
  CHECK_THAT(ev.build.find_volume(n.cap)->parameters.at("p"), WithinRel(4000.0, 1e-12));
}

TEST_CASE("A failed grandchild fails its composites, not their siblings", "[eval][nested]") {
  auto n = filled();
  // Cap's contents reach out of its boundary.
  n.project.document.find_volume(n.cap)->contents =
      nested::fill(core::dbox3::from_min_max({0, 0, 10}, {20, 20, 25}));
  Engine engine(kSettings, threads(4));
  const auto ev = engine.evaluate(n.project);
  CHECK(ev.find(n.cap)->status == VolumeStatus::Failed);
  CHECK(ev.find(n.left)->status == VolumeStatus::Failed);
  CHECK(ev.find(n.housing)->status == VolumeStatus::Failed);
  for (const Id& fine : {n.base, n.core, n.right}) CHECK(ev.find(fine)->status == VolumeStatus::Current);
}

TEST_CASE("Nested output hashes don't depend on the thread count", "[eval][nested]") {
  const auto n = filled();
  const auto hashes = eval::output_hashes_by_threads(n.project, kSettings, {1, 4}, true);
  REQUIRE(hashes.size() == 2);
  CHECK(hashes[0] == hashes[1]);
}

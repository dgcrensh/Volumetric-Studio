// L5: the engine evaluates the active configuration, and switching between
// configurations regenerates only what differs (plan
// docs/plans/L5-configurations-and-design-tables.md, test 4).

#include <filesystem>
#include <set>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "bracket.hpp"
#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/configuration.hpp"

using namespace vs;
using eval::Engine;
using model::Configuration;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

eval::EngineOptions threads(std::size_t n) {
  eval::EngineOptions o;
  o.threads = n;
  return o;
}

std::set<std::string> regenerated(const eval::Evaluation& ev) {
  std::set<std::string> out;
  for (std::size_t i = 0; i < ev.volumes.size(); ++i) {
    if (ev.volumes[i].regenerated) out.insert(ev.document.volumes[i].name);
  }
  return out;
}

double volume_of(const eval::Evaluation& ev, std::size_t i) {
  const auto& c = ev.volumes[i].contents;
  REQUIRE(c);
  REQUIRE_FALSE(c->outputs.empty());
  REQUIRE(c->outputs[0].value);
  return std::get<double>(*c->outputs[0].value);
}

}  // namespace

TEST_CASE("Switching configurations regenerates only what differs", "[eval][configuration]") {
  auto r = scenes::row(3);
  auto& doc = r.project.document;
  doc.configurations.push_back({core::Id::random(), "Shallow", {{r.depth[0], "5"}}});
  doc.configurations.push_back(
      {core::Id::random(), "Deep", {{r.depth[0], "8"}, {r.depth[1], "8"}}});
  // Repeats the authored depth of V2: the same inputs as no configuration.
  doc.configurations.push_back({core::Id::random(), "Same", {{r.depth[2], "10"}}});
  const auto activate = [&](std::size_t k) { doc.active_configuration = doc.configurations[k].id; };

  Engine engine(kSettings, threads(2));
  const auto authored = engine.evaluate(r.project);
  REQUIRE(authored.ok());
  CHECK(authored.configuration.is_null());
  CHECK(regenerated(authored) == std::set<std::string>{"V0", "V1", "V2"});
  CHECK_THAT(volume_of(authored, 0), Catch::Matchers::WithinRel(2560.0, 1e-9));

  activate(0);
  const auto shallow = engine.evaluate(r.project);
  REQUIRE(shallow.ok());
  CHECK(shallow.configuration == doc.configurations[0].id);
  CHECK(regenerated(shallow) == std::set<std::string>{"V0"});
  CHECK_THAT(volume_of(shallow, 0), Catch::Matchers::WithinRel(16.0 * 16 * 5, 1e-9));
  // The evaluated document carries the values, and no configurations.
  CHECK(shallow.document.volumes[0].parameters[0].value == feature::Param::of(5));
  CHECK(shallow.document.configurations.empty());

  activate(1);
  const auto deep = engine.evaluate(r.project);
  REQUIRE(deep.ok());
  CHECK(regenerated(deep) == std::set<std::string>{"V0", "V1"});

  // Back: everything is in the cache.
  activate(0);
  const auto again = engine.evaluate(r.project);
  CHECK(again.stats.regenerated == 0);
  CHECK(again.stats.cache_hits == 3);
  CHECK(again.output_hash() == shallow.output_hash());

  // A configuration that agrees with the authored values shares their keys.
  activate(2);
  const auto same = engine.evaluate(r.project);
  CHECK(same.stats.regenerated == 0);
  doc.active_configuration = core::Id::null();
  const auto back = engine.evaluate(r.project);
  CHECK(back.stats.regenerated == 0);
  CHECK(back.output_hash() == authored.output_hash());
  CHECK(same.output_hash() == authored.output_hash());
}

TEST_CASE("Configurations share the disk cache", "[eval][configuration][cache]") {
  scenes::TempDir dir;
  auto r = scenes::row(2);
  auto& doc = r.project.document;
  doc.configurations.push_back({core::Id::random(), "Shallow", {{r.depth[0], "4"}}});
  doc.active_configuration = doc.configurations[0].id;
  eval::EngineOptions eo = threads(2);
  eo.disk_cache = std::filesystem::path(dir.path) / "cache";
  {
    Engine first(kSettings, eo);
    REQUIRE(first.evaluate(r.project).ok());
  }
  Engine second(kSettings, eo);
  const auto ev = second.evaluate(r.project);
  REQUIRE(ev.ok());
  CHECK(ev.stats.regenerated == 0);
  CHECK(ev.stats.disk_hits == 2);
}

TEST_CASE("The bracket's configurations never regenerate its static volume",
          "[eval][configuration]") {
  auto b = bracket::make_bracket();
  auto& doc = b.project.document;
  doc.configurations.push_back({core::Id::random(), "Thin", {{b.base_thickness, "6"}}});
  doc.configurations.push_back(
      {core::Id::random(), "Thinner", {{b.base_thickness, "base_h / 4"}}});
  Engine engine(kSettings, threads(2));
  REQUIRE(engine.evaluate(b.project).ok());
  for (std::size_t k = 0; k < 2; ++k) {
    doc.active_configuration = doc.configurations[k].id;
    const auto ev = engine.evaluate(b.project);
    INFO(bracket::codes(ev.diagnostics));
    REQUIRE(ev.ok());
    CHECK(regenerated(ev) == std::set<std::string>{"Base"});
  }
}

TEST_CASE("A broken configuration stops the evaluation like any check error",
          "[eval][configuration]") {
  auto r = scenes::row(1);
  auto& doc = r.project.document;
  doc.configurations.push_back({core::Id::random(), "Bad", {{r.depth[0], "1 +"}}});
  doc.active_configuration = doc.configurations[0].id;
  Engine engine(kSettings, threads(1));
  const auto ev = engine.evaluate(r.project);
  CHECK_FALSE(ev.checked);
  CHECK_FALSE(ev.ok());
  CHECK(ev.stats.regenerated == 0);
}

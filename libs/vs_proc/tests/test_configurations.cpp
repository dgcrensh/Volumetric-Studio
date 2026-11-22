// L5: configurations drive procedure parameters, for a procedural volume's
// contents and for a composite's generated children (plan
// docs/plans/L5-configurations-and-design-tables.md, test 5).

#include <catch2/catch_test_macros.hpp>

#include "rack.hpp"
#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/proc/generator.hpp"

using namespace vs;
using namespace vs::proc;
using core::Id;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

std::size_t generated_children(const eval::Evaluation& ev, const Id& parent) {
  std::size_t n = 0;
  for (const auto& v : ev.document.volumes) {
    if (v.generated && v.parent == parent) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("A configuration drives a procedural volume's parameter", "[proc][configuration]") {
  auto b = scenes::boxes({scenes::shell_and_ball().graph});
  auto& doc = b.project.document;
  doc.configurations.push_back({Id::random(), "Thick", {{b.wall[0], "3"}}});
  doc.configurations.push_back({Id::random(), "Thin", {{b.wall[0], "wall_mm"}}});
  // An expression: a layout variable the configuration's value names.
  (void)doc.layout.variables().add("wall_mm", "1.5");

  eval::Engine engine(kSettings);
  install(engine);
  const auto authored = engine.evaluate(b.project);
  INFO(scenes::codes(authored.diagnostics));
  REQUIRE(authored.ok());

  doc.active_configuration = doc.configurations[0].id;
  const auto thick = engine.evaluate(b.project);
  REQUIRE(thick.ok());
  CHECK(thick.stats.regenerated == 1);
  CHECK(thick.volumes[0].contents->output_hash != authored.volumes[0].contents->output_hash);

  doc.active_configuration = doc.configurations[1].id;
  const auto thin = engine.evaluate(b.project);
  INFO(scenes::codes(thin.diagnostics));
  REQUIRE(thin.ok());
  CHECK(thin.stats.regenerated == 1);
  CHECK(thin.document.volumes[0].parameters[0].value == feature::Param::expr("wall_mm"));

  // Back: the cache answers, with the same mesh.
  doc.active_configuration = doc.configurations[0].id;
  const auto again = engine.evaluate(b.project);
  CHECK(again.stats.regenerated == 0);
  CHECK(again.output_hash() == thick.output_hash());
}

TEST_CASE("A configuration changes how many children a generator makes",
          "[proc][configuration][children]") {
  rack::Rack r = rack::make_rack();
  auto& doc = r.project.document;
  // children.grid's count from the Rack's parameter `columns`.
  Graph g;
  g.parameters.push_back({"columns", 3.0});
  const Id columns = g.add("input.parameter", "Columns", {{"name", Setting(std::string("columns"))}});
  const Id count = g.add("vector.compose", "Count", {{"y", Setting(2.0)}, {"z", Setting(1.0)}});
  const Id grid = g.add("children.grid", "Grid", {{"template", Setting(std::string("Cell"))}});
  const Id seat = g.add("children.delegate", "Seat", {{"surface", Setting(r.seat.to_string())}});
  const Id out = g.add("output.children", "Children");
  g.link(columns, "value", count, "x");
  g.link(count, "vector", grid, "count");
  g.link(grid, "children", seat, "children");
  g.link(seat, "children", out, "children");
  model::Volume* rack_volume = doc.find_volume(r.rack);
  std::get<model::CompositeContents>(rack_volume->contents).generator = to_json(g);
  const Id param = Id::random();
  rack_volume->parameters.push_back({param, "columns", feature::Param::of(3)});
  doc.configurations.push_back({Id::random(), "Two", {{param, "2"}}});
  // (Cells narrower than the template's 20 mm wouldn't contain it.)
  doc.configurations.push_back({Id::random(), "One", {{param, "1"}}});

  eval::Engine engine(kSettings);
  install(engine);
  const auto three = engine.evaluate(r.project);
  INFO(scenes::codes(three.diagnostics));
  REQUIRE(three.ok());
  CHECK(generated_children(three, r.rack) == 6);

  doc.active_configuration = doc.configurations[0].id;
  const auto two = engine.evaluate(r.project);
  INFO(scenes::codes(two.diagnostics));
  REQUIRE(two.ok());
  CHECK(generated_children(two, r.rack) == 4);

  doc.active_configuration = doc.configurations[1].id;
  const auto one = engine.evaluate(r.project);
  INFO(scenes::codes(one.diagnostics));
  REQUIRE(one.ok());
  CHECK(generated_children(one, r.rack) == 2);
  const auto* rack_result = one.find(r.rack);
  REQUIRE(rack_result != nullptr);
  CHECK(rack_result->status == model::VolumeStatus::Current);
}

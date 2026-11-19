#include <filesystem>
#include <fstream>
#include <iterator>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "bracket.hpp"
#include "scenes.hpp"
#include "vs/eval/cache.hpp"
#include "vs/eval/engine.hpp"
#include "vs/kernel/exchange.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/model/build.hpp"

using namespace vs;
using vs::eval::cache_key;

namespace {
const core::ProductSettings kSettings = core::ProductSettings::defaults();
}

TEST_CASE("BRep text round-trips a solid and hashes stably", "[eval][cache]") {
  auto box = kernel::make_box(core::dbox3::from_min_max({0, 0, 0}, {10, 20, 30}), kSettings);
  REQUIRE(box.ok());
  const std::string text = kernel::write_brep(*box);
  REQUIRE_FALSE(text.empty());
  auto back = kernel::read_brep(text);
  REQUIRE(back.ok());
  auto solid = kernel::Solid::from(*back);
  REQUIRE(solid.ok());
  CHECK(kernel::bounding_box(*solid).max.z == 30.0);
  CHECK(kernel::shape_hash(*box) == kernel::shape_hash(*box));
  CHECK(kernel::shape_hash(*solid) == kernel::shape_hash(*box));
  CHECK(kernel::write_brep(kernel::Shape{}).empty());
  CHECK(kernel::read_brep("").ok());
  CHECK_FALSE(kernel::read_brep("not brep at all").ok());
}

TEST_CASE("Cache keys follow exactly the volume's inputs", "[eval][cache]") {
  scenes::Row r = scenes::row(2);
  const auto keys = [&](const model::Project& p) {
    const auto layout = model::build_layout(p.document, kSettings);
    return std::pair{cache_key(p.document, layout, 0), cache_key(p.document, layout, 1)};
  };
  const auto [a0, b0] = keys(r.project);
  REQUIRE(a0);
  REQUIRE(b0);
  CHECK(*a0 != *b0);

  SECTION("stable across rebuilds") {
    const auto [a1, b1] = keys(r.project);
    CHECK(*a1 == *a0);
    CHECK(*b1 == *b0);
  }
  SECTION("a parameter changes its own volume's key only") {
    scenes::set_parameter(r.project, r.volumes[0], r.depth[0], 12);
    const auto [a1, b1] = keys(r.project);
    CHECK(*a1 != *a0);
    CHECK(*b1 == *b0);
  }
  SECTION("so does the seed") {
    r.project.document.volumes[1].seed = 99;
    const auto [a1, b1] = keys(r.project);
    CHECK(*a1 == *a0);
    CHECK(*b1 != *b0);
  }
  SECTION("a renamed volume keeps its key: names aren't inputs") {
    r.project.document.volumes[0].name = "Renamed";
    CHECK(*keys(r.project).first == *a0);
  }
  SECTION("a failed boundary has no key") {
    // Collapse V1's box: its max corner onto its min corner.
    auto& box = std::get<model::BoxBoundary>(r.project.document.volumes[1].boundary);
    box.max = box.min;
    const auto [a1, b1] = keys(r.project);
    CHECK(a1);
    CHECK_FALSE(b1);
  }
}

TEST_CASE("A static volume's key ignores its parameters", "[eval][cache]") {
  auto b = bracket::make_bracket();
  const auto key = [&] {
    const auto layout = model::build_layout(b.project.document, kSettings);
    return *cache_key(b.project.document, layout, 1);  // Upright, static
  };
  const auto before = key();
  scenes::set_parameter(b.project, b.upright, b.upright_load, 123);
  CHECK(key() == before);
}

TEST_CASE("The disk cache serves a second engine", "[eval][cache]") {
  scenes::TempDir dir;
  auto r = scenes::row(3);
  eval::EngineOptions eo;
  eo.threads = 2;
  eo.disk_cache = std::filesystem::path(dir.path) / "cache";

  eval::Engine first(kSettings, eo);
  const auto a = first.evaluate(r.project);
  REQUIRE(a.ok());
  CHECK(a.stats.regenerated == 3);
  CHECK(std::distance(std::filesystem::directory_iterator(eo.disk_cache),
                      std::filesystem::directory_iterator{}) == 6);  // .brep and .json each

  eval::Engine second(kSettings, eo);
  const auto b = second.evaluate(r.project);
  REQUIRE(b.ok());
  CHECK(b.stats.regenerated == 0);
  CHECK(b.stats.disk_hits == 3);
  for (const auto& v : b.volumes) {
    CHECK(v.status == model::VolumeStatus::Current);
    CHECK(v.from_disk);
    REQUIRE(v.contents);
    REQUIRE(v.contents->outputs.size() == 1);
    CHECK_THAT(std::get<double>(*v.contents->outputs[0].value), Catch::Matchers::WithinRel(2560.0, 1e-12));
  }
  CHECK(b.output_hash() == a.output_hash());

  SECTION("a damaged entry is a miss, not an error") {
    for (const auto& e : std::filesystem::directory_iterator(eo.disk_cache)) {
      if (e.path().extension() == ".json") {
        std::ofstream(e.path(), std::ios::trunc) << "{ not json";
      }
    }
    eval::Engine third(kSettings, eo);
    const auto c = third.evaluate(r.project);
    CHECK(c.ok());
    CHECK(c.stats.regenerated == 3);
    CHECK(c.output_hash() == a.output_hash());
  }
}

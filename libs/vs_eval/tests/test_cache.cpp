#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "bracket.hpp"
#include "scenes.hpp"
#include "vs/core/version.hpp"
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
  const auto keys = [&](const model::Project& p, const core::ProductSettings& s = kSettings) {
    const auto layout = model::build_layout(p.document, s);
    return std::pair{cache_key(p.document, layout, 0, s), cache_key(p.document, layout, 1, s)};
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
  SECTION("other product settings change every key (H3)") {
    core::ProductSettings tighter = kSettings;
    tighter.linear_tolerance_mm = 1e-7;
    const auto [a1, b1] = keys(r.project, tighter);
    CHECK(*a1 != *a0);
    CHECK(*b1 != *b0);
    core::ProductSettings larger = kSettings;
    larger.working_half_extent_mm = 2000.0;
    const auto [a2, b2] = keys(r.project, larger);
    CHECK(*a2 != *a0);
    CHECK(*b2 != *b0);
    CHECK(*a2 != *a1);
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
    return *cache_key(b.project.document, layout, 1, kSettings);  // Upright, static
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
  SECTION("entries an older engine wrote are a miss (H3: engine 1 to 2)") {
    for (const auto& e : std::filesystem::directory_iterator(eo.disk_cache)) {
      if (e.path().extension() != ".json") continue;
      std::string text;
      {
        std::ifstream in(e.path());
        text.assign(std::istreambuf_iterator<char>(in), {});
      }
      const std::string now = "\"engine\": " + std::to_string(core::kEngineVersion) + ",";
      const auto at = text.find(now);
      REQUIRE(at != std::string::npos);
      text.replace(at, now.size(), "\"engine\": 1,");
      std::ofstream(e.path(), std::ios::trunc) << text;
    }
    eval::Engine third(kSettings, eo);
    const auto c = third.evaluate(r.project);
    REQUIRE(c.ok());
    CHECK(c.stats.disk_hits == 0);
    CHECK(c.stats.regenerated == 3);
    CHECK(c.output_hash() == a.output_hash());
  }
  SECTION("an engine with other product settings misses it (H3)") {
    core::ProductSettings other = kSettings;
    other.linear_tolerance_mm = 1e-7;
    eval::Engine third(other, eo);
    const auto c = third.evaluate(r.project);
    REQUIRE(c.ok());
    CHECK(c.stats.disk_hits == 0);
    CHECK(c.stats.regenerated == 3);
  }
}

// --- project caches (M17) -----------------------------------------------------------

TEST_CASE("Mesh contents round-trip through cache files bit for bit", "[eval][cache]") {
  auto contents = std::make_shared<eval::GeneratedContents>();
  contents->key = core::blake3_of_bytes(std::span<const std::byte>{});
  mesh::Mesh m;
  const core::Id volume = core::Id::random();
  const core::Id seam = core::Id::random();
  m.add_vertex({0.1f, 0.2f, 0.3f}, seam);
  m.add_vertex({1.0f / 3.0f, 0, 0});
  m.add_vertex({0, 7.25f, -1e-7f});
  m.add_triangle(0, 1, 2, volume);
  contents->mesh = m;
  contents->output_hash = core::blake3_of_bytes(std::span<const std::byte>{});
  model::OutputResult o;
  o.output = core::Id::random();
  o.value = 12.5;
  contents->outputs.push_back(o);
  contents->diagnostics.add(core::Diagnostic::warning("proc.mesh.seam_uncovered", "a warning"));

  model::CacheFiles files;
  REQUIRE(eval::encode_entry(*contents, files));
  CHECK(files.size() == 2);
  const auto back = eval::decode_entry(files, contents->key);
  REQUIRE(back);
  REQUIRE(back->mesh);
  CHECK(*back->mesh == m);
  CHECK(back->output_hash == contents->output_hash);
  CHECK(std::get<double>(*back->outputs.at(0).value) == 12.5);
  CHECK(back->diagnostics.items().at(0).code == "proc.mesh.seam_uncovered");

  SECTION("a damaged mesh is a miss") {
    auto& bytes = files.at(contents->key.to_hex() + ".mesh");
    bytes.resize(bytes.size() - 1);
    CHECK_FALSE(eval::decode_entry(files, contents->key));
  }
  SECTION("failed contents aren't stored") {
    auto failed = std::make_shared<eval::GeneratedContents>(*contents);
    failed->status = model::VolumeStatus::Failed;
    model::CacheFiles none;
    CHECK_FALSE(eval::encode_entry(*failed, none));
    CHECK(none.empty());
  }
}

TEST_CASE("Project caches let a new engine skip what still matches", "[eval][cache]") {
  auto b = bracket::make_bracket();
  eval::EngineOptions eo;
  eo.threads = 2;
  eval::Engine first(kSettings, eo);
  const auto a = first.evaluate(b.project);
  REQUIRE(a.ok());

  // The Upright (static STEP) is stored; the Base (parametric) rebuilds.
  const model::CacheFiles files = eval::project_cache_files(a);
  CHECK(files.size() == 2);
  CHECK(files.contains(a.find(b.upright)->key->to_hex() + ".json"));

  eval::Engine second(kSettings, eo);
  CHECK(second.cache().insert_files(files) == 1);
  const auto c = second.evaluate(b.project);
  REQUIRE(c.ok());
  CHECK(c.find(b.upright)->from_cache);
  CHECK_FALSE(c.find(b.base)->from_cache);
  CHECK(c.output_hash() == a.output_hash());

  SECTION("stale entries are simply not used") {
    // A taller Upright: its boundary and so its key change, so the stored
    // entry is never found and the file is read again.
    eval::Engine third(kSettings, eo);
    third.cache().insert_files(files);
    REQUIRE_FALSE(b.project.document.layout.variables().set_expression(b.upright_h, "41 mm").has_errors());
    const auto d = third.evaluate(b.project);
    CHECK_FALSE(d.find(b.upright)->from_cache);
    CHECK(d.find(b.upright)->key != a.find(b.upright)->key);
  }
}

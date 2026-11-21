// H4 (plan docs/plans/H-hardening.md, decision 0026): cache entries and
// mesh measures that failed on main before their fixes.

#include <cmath>
#include <map>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "scenes.hpp"
#include "vs/eval/cache.hpp"
#include "vs/eval/engine.hpp"
#include "vs/eval/measure.hpp"
#include "vs/eval/thread_pool.hpp"

using namespace vs;
using Catch::Matchers::WithinRel;

namespace {

// An axis-aligned box from (lo) to (hi) in FP32, two triangles a face,
// wound counter-clockwise from outside.
mesh::Mesh box(const core::dvec3& lo, const core::dvec3& hi) {
  mesh::Mesh m;
  for (int k = 0; k < 8; ++k) {
    m.add_vertex(core::to_fp32(core::dvec3{(k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y,
                                           (k & 4) ? hi.z : lo.z}));
  }
  const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                           {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (const auto& f : faces) {
    const auto v = [&](int i) { return static_cast<std::uint32_t>(f[i]); };
    m.add_triangle(v(0), v(1), v(2), core::Id::null());
    m.add_triangle(v(0), v(2), v(3), core::Id::null());
  }
  return m;
}

}  // namespace

TEST_CASE("Mesh measures keep their precision far from the origin", "[eval][measure][hardening]") {
  const core::dvec3 lo{987.1, -993.3, 999.7}, hi{987.2, -993.2, 999.8};
  const mesh::Mesh m = box(lo, hi);
  const core::dvec3 a = core::to_fp64(m.positions[0]), b = core::to_fp64(m.positions[7]);
  const core::dvec3 size = b - a;
  const double exact = size.x * size.y * size.z;
  model::Volume v;
  v.outputs = {{core::Id::random(), "volume", model::OutputQuantity::Volume, 1.0},
               {core::Id::random(), "centre", model::OutputQuantity::CentreOfGravity, 1.0}};
  eval::ThreadPool pool(1);
  const auto out = eval::measure_mesh(v, m, pool);
  REQUIRE(out.size() == 2);
  REQUIRE(out[0].value);
  CHECK_THAT(std::get<double>(*out[0].value), WithinRel(exact, 1e-9));
  REQUIRE(out[1].value);
  const core::dvec3 centre = std::get<core::dvec3>(*out[1].value);
  const core::dvec3 want = (a + b) * 0.5;
  CHECK(core::length(centre - want) < 1e-9);
}

TEST_CASE("A cache entry with enumerations out of range is a miss", "[eval][cache][hardening]") {
  auto contents = std::make_shared<eval::GeneratedContents>();
  contents->key = core::hash_of(std::string("h4 entry"));
  contents->mesh = box({0, 0, 0}, {1, 1, 1});
  contents->diagnostics.add(core::Diagnostic::warning(
      "eval.test", "a note", {core::SubjectRef{core::SubjectKind::Volume, core::Id::random(), "V"}}));
  contents->output_hash = eval::output_hash(contents->status, {}, &*contents->mesh, {});
  model::CacheFiles files;
  REQUIRE(eval::encode_entry(*contents, files));
  REQUIRE(eval::decode_entry(files, contents->key) != nullptr);

  const std::string meta = contents->key.to_hex() + ".json";
  const auto with = [&](const std::string& from, const std::string& to) {
    model::CacheFiles changed = files;
    std::string text(reinterpret_cast<const char*>(changed.at(meta).data()), changed.at(meta).size());
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    changed[meta] = model::Blob(reinterpret_cast<const std::byte*>(text.data()),
                                reinterpret_cast<const std::byte*>(text.data()) + text.size());
    return changed;
  };
  const auto severity = std::to_string(static_cast<int>(core::Severity::Warning));
  const auto kind = std::to_string(static_cast<int>(core::SubjectKind::Volume));
  CHECK(eval::decode_entry(with("\"severity\": " + severity, "\"severity\": 7"), contents->key) == nullptr);
  CHECK(eval::decode_entry(with("\"severity\": " + severity, "\"severity\": -1"), contents->key) == nullptr);
  CHECK(eval::decode_entry(with("\"kind\": " + kind, "\"kind\": 99"), contents->key) == nullptr);
  CHECK(eval::decode_entry(with("\"severity\": " + severity, "\"severity\": " + severity), contents->key) != nullptr);
}

// --- H5: the memory cache is bounded ------------------------------------------------------

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

eval::ContentsPtr mesh_entry(int i, std::size_t triangles) {
  auto c = std::make_shared<eval::GeneratedContents>();
  c->key = core::hash_of(std::string_view("h5 contents"), static_cast<std::uint64_t>(i));
  mesh::Mesh m;
  for (std::size_t t = 0; t < triangles; ++t) {
    const auto f = static_cast<float>(t);
    const auto a = m.add_vertex({f, 0, 0});
    const auto b = m.add_vertex({f, 1, 0});
    const auto d = m.add_vertex({f, 0, 1});
    m.add_triangle(a, b, d, core::Id::null());
  }
  c->mesh = std::move(m);
  return c;
}

}  // namespace

TEST_CASE("The memory cache forgets the least recently used contents past its budget (H5)",
          "[eval][cache][hardening]") {
  const std::size_t one = eval::contents_bytes(*mesh_entry(0, 1000));
  REQUIRE(one > 1000 * 3 * sizeof(core::fvec3));
  eval::ContentsCache cache(3 * one + one / 2);  // room for three
  for (int i = 0; i < 3; ++i) cache.insert(mesh_entry(i, 1000));
  CHECK(cache.memory_size() == 3);
  CHECK(cache.memory_bytes() == 3 * one);
  REQUIRE(cache.find(mesh_entry(0, 0)->key));  // 0 is now the most recently used
  cache.insert(mesh_entry(3, 1000));
  CHECK(cache.memory_size() == 3);
  CHECK(cache.memory_bytes() <= cache.memory_budget());
  CHECK(cache.find(mesh_entry(0, 0)->key));
  CHECK_FALSE(cache.find(mesh_entry(1, 0)->key));
  CHECK(cache.find(mesh_entry(2, 0)->key));
  CHECK(cache.find(mesh_entry(3, 0)->key));

  // Contents bigger than the whole budget still stay, alone: the engine
  // just made them, and evaluations share them anyway.
  cache.insert(mesh_entry(4, 10000));
  CHECK(cache.memory_size() == 1);
  CHECK(cache.find(mesh_entry(4, 0)->key));

  // Replacing an entry counts it once; clearing counts nothing.
  cache.insert(mesh_entry(4, 10000));
  CHECK(cache.memory_bytes() == eval::contents_bytes(*mesh_entry(4, 10000)));
  cache.clear_memory();
  CHECK(cache.memory_bytes() == 0);
}

TEST_CASE("An engine evaluates within a small memory cache, regenerating what it forgot (H5)",
          "[eval][cache][hardening]") {
  auto b = scenes::row(3);
  eval::EngineOptions eo;
  eo.threads = 2;
  eo.memory_cache_bytes = 1;  // keeps only the newest entry
  eval::Engine engine(kSettings, eo);
  const auto first = engine.evaluate(b.project);
  REQUIRE(first.ok());
  CHECK(engine.cache().memory_size() == 1);
  // Unchanged volumes are still current (the engine keeps what it shows);
  // the result is the same as with an unbounded cache.
  eval::Engine roomy(kSettings, {2});
  const auto reference = roomy.evaluate(b.project);
  const auto second = engine.evaluate(b.project);
  REQUIRE(second.ok());
  REQUIRE(second.volumes.size() == reference.volumes.size());
  for (std::size_t i = 0; i < second.volumes.size(); ++i) {
    CHECK(second.volumes[i].status == reference.volumes[i].status);
    CHECK(second.volumes[i].contents->output_hash == reference.volumes[i].contents->output_hash);
  }
}

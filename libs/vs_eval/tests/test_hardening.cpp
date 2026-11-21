// H4 (plan docs/plans/H-hardening.md, decision 0026): cache entries and
// mesh measures that failed on main before their fixes.

#include <cmath>
#include <map>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "vs/eval/cache.hpp"
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

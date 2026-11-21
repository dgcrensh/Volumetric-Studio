#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iterator>
#include <set>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "bracket.hpp"
#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/eval/generator.hpp"
#include "vs/kernel/exchange.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/validate/registry.hpp"

using namespace vs;
using eval::Engine;
using eval::EngineOptions;
using eval::EvaluateOptions;
using model::VolumeStatus;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

EngineOptions threads(std::size_t n) {
  EngineOptions o;
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

double output_value(const eval::Evaluation& ev, std::size_t volume) {
  const auto& c = ev.volumes[volume].contents;
  REQUIRE(c);
  REQUIRE_FALSE(c->outputs.empty());
  REQUIRE(c->outputs[0].value);
  return std::get<double>(*c->outputs[0].value);
}

}  // namespace

TEST_CASE("A first evaluation generates every volume", "[eval][engine]") {
  auto r = scenes::row(4);
  Engine engine(kSettings, threads(4));
  const auto ev = engine.evaluate(r.project);
  INFO(bracket::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  CHECK(ev.checked);
  CHECK(ev.stats.regenerated == 4);
  CHECK(ev.stats.cache_hits == 0);
  for (std::size_t i = 0; i < 4; ++i) {
    CHECK(ev.volumes[i].status == VolumeStatus::Current);
    CHECK(ev.build.volumes[i].status == VolumeStatus::Current);
    CHECK_FALSE(ev.build.volumes[i].contents.is_empty());
    CHECK_THAT(output_value(ev, i), Catch::Matchers::WithinRel(2560.0, 1e-12));
  }
  for (const auto& [id, status] : engine.states()) CHECK(status == VolumeStatus::Current);

  // Nothing changed: nothing regenerates.
  const auto again = engine.evaluate(r.project);
  CHECK(again.stats.regenerated == 0);
  CHECK(again.stats.cache_hits == 4);
  CHECK(again.output_hash() == ev.output_hash());
}

TEST_CASE("Changing a parameter regenerates only that volume and its dependants",
          "[eval][engine][acceptance]") {
  auto r = scenes::chain();  // A -> B by a link; C independent
  Engine engine(kSettings, threads(4));
  const auto first = engine.evaluate(r.project);
  INFO(bracket::codes(first.diagnostics));
  REQUIRE(first.ok());
  CHECK(regenerated(first) == std::set<std::string>{"A", "B", "C"});
  CHECK(first.stats.waves == 2);  // A and C, then B
  CHECK_THAT(output_value(first, 1), Catch::Matchers::WithinRel(2560.0, 1e-12));  // B: feed = A's 2560 → 10 mm deep

  SECTION("A's depth: A and B (its dependant) regenerate, C doesn't") {
    scenes::set_parameter(r.project, r.volumes[0], r.depth[0], 15);
    const auto ev = engine.evaluate(r.project);
    REQUIRE(ev.ok());
    CHECK(regenerated(ev) == std::set<std::string>{"A", "B"});
    CHECK(ev.stats.regenerated == 2);
    CHECK(ev.stats.cache_hits == 1);
    CHECK_THAT(output_value(ev, 0), Catch::Matchers::WithinRel(3840.0, 1e-12));
    CHECK_THAT(output_value(ev, 1), Catch::Matchers::WithinRel(3840.0, 1e-12));  // the link carried A's new volume
    // Its parameter took the linked value.
    CHECK(ev.document.volumes[1].parameters[0].value.value == std::get<double>(*ev.volumes[0].contents->outputs[0].value));
  }
  SECTION("C's depth: only C") {
    scenes::set_parameter(r.project, r.volumes[2], r.depth[2], 5);
    const auto ev = engine.evaluate(r.project);
    REQUIRE(ev.ok());
    CHECK(regenerated(ev) == std::set<std::string>{"C"});
    CHECK(ev.stats.cache_hits == 2);
  }
  SECTION("B's own parameter is the link's: B's inputs don't change") {
    scenes::set_parameter(r.project, r.volumes[1], r.depth[1], 1000);
    const auto ev = engine.evaluate(r.project);
    CHECK(regenerated(ev).empty());
  }
  SECTION("back to an earlier value: everything comes from the cache") {
    scenes::set_parameter(r.project, r.volumes[0], r.depth[0], 15);
    (void)engine.evaluate(r.project);
    scenes::set_parameter(r.project, r.volumes[0], r.depth[0], 10);
    const auto ev = engine.evaluate(r.project);
    CHECK(regenerated(ev).empty());
    CHECK(ev.output_hash() == first.output_hash());
  }
}

TEST_CASE("The bracket's static volume doesn't regenerate when its linked parameter changes",
          "[eval][engine]") {
  auto b = bracket::make_bracket();
  Engine engine(kSettings, threads(2));
  const auto first = engine.evaluate(b.project);
  INFO(bracket::codes(first.diagnostics));
  REQUIRE(first.ok());
  CHECK(first.stats.regenerated == 2);
  // The link fed Base's mass to Upright's load.
  const auto* upright = first.document.find_volume(b.upright);
  REQUIRE(upright != nullptr);
  CHECK(upright->parameters[0].value.value > 29.0);

  // Base thinner: its mass changes, and so does Upright's load, but a
  // static file can't change with a parameter.
  model::Volume* base = b.project.document.find_volume(b.base);
  base->parameters[0].value = feature::Param::of(8);
  const auto ev = engine.evaluate(b.project);
  INFO(bracket::codes(ev.diagnostics));
  CHECK(regenerated(ev) == std::set<std::string>{"Base"});
}

TEST_CASE("Output hashes are identical with 1, 4 and 16 threads", "[eval][engine][acceptance]") {
  SECTION("twelve parametric volumes") {
    auto r = scenes::row(12);
    const auto hashes = eval::output_hashes_by_threads(r.project, kSettings, {1, 4, 16}, true);
    REQUIRE(hashes.size() == 3);
    CHECK(hashes[0] == hashes[1]);
    CHECK(hashes[0] == hashes[2]);
  }
  SECTION("the bracket, welded") {
    auto b = bracket::make_bracket();
    const auto hashes = eval::output_hashes_by_threads(b.project, kSettings, {1, 4, 16}, true);
    CHECK(hashes[0] == hashes[1]);
    CHECK(hashes[0] == hashes[2]);
  }
  SECTION("a chain with links") {
    auto r = scenes::chain();
    const auto hashes = eval::output_hashes_by_threads(r.project, kSettings, {1, 4, 16});
    CHECK(hashes[0] == hashes[1]);
    CHECK(hashes[0] == hashes[2]);
  }
}

TEST_CASE("The determinism check regenerates twice and finds nothing", "[eval][engine]") {
  auto b = bracket::make_bracket();
  EngineOptions o = threads(4);
  o.check_determinism = true;
  Engine engine(kSettings, o);
  EvaluateOptions eo;
  eo.weld = true;
  const auto ev = engine.evaluate(b.project, eo);
  INFO(bracket::codes(ev.diagnostics));
  CHECK(ev.ok());
  CHECK(ev.stats.regenerated == 4);  // two volumes, twice each
  for (const auto& d : ev.diagnostics) CHECK(d.code != "eval.nondeterministic");
  REQUIRE(ev.combined);
  CHECK(ev.combined->ok());
}

TEST_CASE("A volume forced to fail leaves siblings current and its own contents stale",
          "[eval][engine][acceptance]") {
  auto r = scenes::chain();
  Engine engine(kSettings, threads(4));
  const auto good = engine.evaluate(r.project);
  REQUIRE(good.ok());
  const auto good_contents = good.volumes[0].contents;

  SECTION("contents that leave the boundary") {
    scenes::set_parameter(r.project, r.volumes[0], r.depth[0], 25);  // 5 mm out of the bottom
    const auto ev = engine.evaluate(r.project);
    CHECK_FALSE(ev.ok());
    const auto& a = ev.volumes[0];
    CHECK(a.status == VolumeStatus::Failed);
    CHECK(a.stale_contents);
    CHECK(a.contents == good_contents);  // its last valid contents
    CHECK(bracket::codes(a.diagnostics).find("validate.containment") != std::string::npos);
    CHECK(ev.build.volumes[0].status == VolumeStatus::Failed);
    CHECK_FALSE(ev.build.volumes[0].contents.is_empty());
    // B depends on A: blocked, keeping its contents.
    CHECK(ev.volumes[1].status == VolumeStatus::Stale);
    CHECK(ev.volumes[1].contents == good.volumes[1].contents);
    CHECK(bracket::codes(ev.volumes[1].diagnostics).find("eval.blocked") != std::string::npos);
    CHECK(ev.stats.blocked == 1);
    // C, a sibling, stays current.
    CHECK(ev.volumes[2].status == VolumeStatus::Current);
    CHECK_FALSE(ev.volumes[2].stale_contents);
    CHECK(engine.states().at(r.volumes[2]) == VolumeStatus::Current);
    CHECK(engine.states().at(r.volumes[0]) == VolumeStatus::Failed);

    SECTION("and recovers when fixed") {
      scenes::set_parameter(r.project, r.volumes[0], r.depth[0], 10);
      const auto fixed = engine.evaluate(r.project);
      CHECK(fixed.ok());
      CHECK(fixed.volumes[0].status == VolumeStatus::Current);
      CHECK(fixed.volumes[1].status == VolumeStatus::Current);
      CHECK(regenerated(fixed).empty());  // all from the cache
    }
    SECTION("the same failing inputs don't regenerate again") {
      const auto again = engine.evaluate(r.project);
      CHECK(again.volumes[0].status == VolumeStatus::Failed);
      CHECK(regenerated(again).empty());
    }
  }
  SECTION("a feature tree that fails to rebuild") {
    scenes::set_parameter(r.project, r.volumes[2], r.depth[2], 0);
    const auto ev = engine.evaluate(r.project);
    CHECK(ev.volumes[2].status == VolumeStatus::Failed);
    CHECK(ev.volumes[2].stale_contents);
    CHECK(ev.volumes[2].contents == good.volumes[2].contents);
    CHECK(ev.volumes[0].status == VolumeStatus::Current);
    CHECK(ev.volumes[1].status == VolumeStatus::Current);
  }
  SECTION("a failed boundary") {
    auto& box = std::get<model::BoxBoundary>(r.project.document.volumes[2].boundary);
    box.max = box.min;
    const auto ev = engine.evaluate(r.project);
    CHECK(ev.volumes[2].status == VolumeStatus::Failed);
    CHECK_FALSE(ev.volumes[2].key);
    CHECK(ev.volumes[2].contents == good.volumes[2].contents);
    CHECK(ev.volumes[0].status == VolumeStatus::Current);
  }
}

TEST_CASE("A cancelled evaluation leaves the model consistent", "[eval][engine][acceptance]") {
  auto r = scenes::row(8);
  // One thread, cancelled as the first volume starts: that volume finishes
  // (work under way isn't interrupted) and nothing else starts.
  Engine engine(kSettings, threads(1));
  EvaluateOptions eo;
  eo.progress = [&](const eval::ProgressEvent& e) {
    if (e.kind == eval::ProgressEvent::Kind::VolumeStarted) eo.cancel.cancel();
  };
  const auto ev = engine.evaluate(r.project, eo);
  CHECK(ev.cancelled);
  CHECK(ev.stats.regenerated == 1);
  CHECK_FALSE(ev.combined);
  std::size_t current = 0;
  for (const auto& v : ev.volumes) {
    CHECK((v.status == VolumeStatus::Current || v.status == VolumeStatus::Stale));
    if (v.status == VolumeStatus::Current) {
      ++current;
      REQUIRE(v.contents);
    } else {
      CHECK_FALSE(v.contents);  // never had any
    }
  }
  CHECK(current == 1);
  for (const auto& [id, status] : engine.states()) {
    CHECK(status != VolumeStatus::Regenerating);
  }

  // The next evaluation finishes the job, and matches an uncancelled one.
  const auto rest = engine.evaluate(r.project);
  CHECK_FALSE(rest.cancelled);
  CHECK(rest.ok());
  CHECK(rest.stats.regenerated == 7);
  CHECK(rest.stats.cache_hits == 1);
  Engine fresh(kSettings, threads(4));
  CHECK(fresh.evaluate(r.project).output_hash() == rest.output_hash());

  SECTION("cancelled before anything starts") {
    scenes::set_parameter(r.project, r.volumes[0], r.depth[0], 7);
    EvaluateOptions early;
    early.cancel.cancel();
    const auto none = engine.evaluate(r.project, early);
    CHECK(none.cancelled);
    CHECK(none.stats.regenerated == 0);
    CHECK(none.volumes[0].status == VolumeStatus::Stale);
    CHECK(none.volumes[0].contents == rest.volumes[0].contents);  // the old contents, stale
    CHECK(none.volumes[0].stale_contents);
    CHECK_FALSE(none.build.volumes[0].boundary.is_empty());  // the layout is still built
  }
}

TEST_CASE("The preview tier shows boundaries at once", "[eval][engine]") {
  auto r = scenes::row(3);
  Engine engine(kSettings, threads(2));
  const auto blank = engine.preview(r.project);
  CHECK(blank.stats.regenerated == 0);
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(blank.volumes[i].status == VolumeStatus::Stale);
    CHECK_FALSE(blank.volumes[i].contents);
    CHECK_FALSE(blank.build.volumes[i].boundary.is_empty());
  }
  (void)engine.evaluate(r.project);

  scenes::set_parameter(r.project, r.volumes[1], r.depth[1], 4);
  const auto preview = engine.preview(r.project);
  CHECK(preview.stats.regenerated == 0);
  CHECK(preview.volumes[0].status == VolumeStatus::Current);  // cached
  CHECK(preview.volumes[1].status == VolumeStatus::Stale);    // its old contents, greyed out
  CHECK(preview.volumes[1].stale_contents);
  REQUIRE(preview.volumes[1].contents);
  CHECK_THAT(output_value(preview, 1), Catch::Matchers::WithinRel(2560.0, 1e-12));
}

TEST_CASE("Progress reports each regenerating volume", "[eval][engine]") {
  auto r = scenes::row(5);
  Engine engine(kSettings, threads(4));
  EvaluateOptions eo;
  std::size_t started = 0, finished = 0, total = 0;
  bool done = false;
  eo.progress = [&](const eval::ProgressEvent& e) {
    using K = eval::ProgressEvent::Kind;
    if (e.kind == K::VolumeStarted) ++started;
    if (e.kind == K::VolumeFinished) {
      ++finished;
      CHECK(e.status == VolumeStatus::Current);
      CHECK(e.done == finished);
      total = e.total;
    }
    if (e.kind == K::Finished) done = true;
  };
  (void)engine.evaluate(r.project, eo);
  CHECK(started == 5);
  CHECK(finished == 5);
  CHECK(total == 5);
  CHECK(done);
}

TEST_CASE("A document that breaks the rules isn't evaluated", "[eval][engine]") {
  auto r = scenes::row(2);
  Engine engine(kSettings, threads(1));
  // A link from a volume's output to its own parameter: a loop.
  r.project.document.links.push_back({core::Id::random(), {r.volumes[0], r.output[0]}, r.depth[0]});
  const auto ev = engine.evaluate(r.project);
  CHECK_FALSE(ev.checked);
  CHECK_FALSE(ev.ok());
  CHECK(bracket::codes(ev.diagnostics).find("model.dependency.loop") != std::string::npos);
  CHECK(ev.stats.regenerated == 0);
}

TEST_CASE("Evaluation welds the bracket into one watertight mesh", "[eval][engine]") {
  auto b = bracket::make_bracket();
  Engine engine(kSettings, threads(4));
  EvaluateOptions eo;
  eo.weld = true;
  const auto ev = engine.evaluate(b.project, eo);
  INFO(bracket::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  REQUIRE(ev.combined);
  CHECK(ev.combined->ok());
  CHECK(ev.combined->volumes.size() == 2);
}

TEST_CASE("The engine's diagnostics route to the volume's contents", "[eval][engine]") {
  auto r = scenes::chain();
  Engine engine(kSettings, threads(2));
  (void)engine.evaluate(r.project);
  scenes::set_parameter(r.project, r.volumes[0], r.depth[0], 25);
  const auto ev = engine.evaluate(r.project);
  const auto routed = validate::route_all(ev.diagnostics, ev.document);
  bool blocked = false;
  for (const auto& rd : routed) {
    if (rd.diagnostic.code != "eval.blocked") continue;
    blocked = true;
    CHECK(rd.target.on == validate::ActOn::Contents);
    CHECK(rd.target.volume == r.volumes[1]);
  }
  CHECK(blocked);
}

TEST_CASE("Splitting the build gives what build() gives", "[eval][engine]") {
  auto b = bracket::make_bracket();
  const auto whole = model::build(b.project.document, kSettings, &b.project.blobs);
  auto halves = model::build_layout(b.project.document, kSettings);
  for (std::size_t i = 0; i < halves.volumes.size(); ++i) {
    model::apply_contents(halves.volumes[i], model::build_contents(b.project.document, halves, i,
                                                                   kSettings, &b.project.blobs));
  }
  REQUIRE(whole.volumes.size() == halves.volumes.size());
  for (std::size_t i = 0; i < whole.volumes.size(); ++i) {
    CHECK(whole.volumes[i].status == halves.volumes[i].status);
    CHECK(kernel::shape_hash(whole.volumes[i].contents) ==
          kernel::shape_hash(halves.volumes[i].contents));
    CHECK(bracket::codes(whole.volumes[i].diagnostics) ==
          bracket::codes(halves.volumes[i].diagnostics));
  }
}

// --- plan H1: a nondeterministic generator --------------------------------

namespace {

// Fills each parametric volume with a small tetrahedron inside its boundary,
// moved 0.01 mm further along x on every run: deterministic only when
// `stable`.
class FlakyGenerator : public eval::ContentsGenerator {
 public:
  explicit FlakyGenerator(bool stable) : stable_(stable) {}

  Job prepare(const model::Document& document, const model::DocumentBuild& layout,
              std::size_t index, const core::ProductSettings&, eval::ThreadPool&,
              const eval::CancellationToken&) override {
    const core::dbox3 box = kernel::bounding_box(layout.volumes[index].boundary);
    const core::Id volume = document.volumes[index].id;
    return [this, box, volume](eval::GeneratedContents& out) {
      const double shift = stable_ ? 0.0 : 0.01 * static_cast<double>(runs_.fetch_add(1) % 4);
      const core::dvec3 c = box.center() + core::dvec3{shift, 0, 0};
      mesh::Mesh m;
      const auto v = [&](double x, double y, double z) {
        return m.add_vertex(core::to_fp32(c + core::dvec3{x, y, z}));
      };
      const std::uint32_t a = v(0, 0, 0), b = v(2, 0, 0), d = v(0, 2, 0), e = v(0, 0, 2);
      m.add_triangle(a, d, b, volume);
      m.add_triangle(a, b, e, volume);
      m.add_triangle(a, e, d, volume);
      m.add_triangle(b, d, e, volume);
      out.mesh = std::move(m);
      out.status = model::VolumeStatus::Current;
    };
  }

 private:
  bool stable_;
  std::atomic<int> runs_{0};
};

}  // namespace

TEST_CASE("Nondeterministic contents fail and are never cached", "[eval][engine][h1]") {
  scenes::TempDir dir;
  auto r = scenes::row(1);
  EngineOptions o = threads(2);
  o.check_determinism = true;
  o.disk_cache = std::filesystem::path(dir.path) / "cache";

  SECTION("a stable generator passes") {
    Engine engine(kSettings, o);
    engine.set_generator(model::VolumeKind::Parametric, std::make_shared<FlakyGenerator>(true));
    const auto ev = engine.evaluate(r.project);
    INFO(bracket::codes(ev.diagnostics));
    CHECK(ev.volumes[0].status == VolumeStatus::Current);
    CHECK(ev.diagnostics.with_code("eval.nondeterministic").empty());
  }
  SECTION("a flaky one fails, shows nothing and leaves no cache entry") {
    Engine engine(kSettings, o);
    engine.set_generator(model::VolumeKind::Parametric, std::make_shared<FlakyGenerator>(false));
    const auto ev = engine.evaluate(r.project);
    CHECK_FALSE(ev.diagnostics.with_code("eval.nondeterministic").empty());
    CHECK(ev.volumes[0].status == VolumeStatus::Failed);
    CHECK_FALSE(ev.ok());
    const auto files = std::filesystem::exists(o.disk_cache)
                           ? std::distance(std::filesystem::directory_iterator(o.disk_cache),
                                           std::filesystem::directory_iterator{})
                           : 0;
    CHECK(files == 0);
    // Evaluating again finds the failure in memory, not contents to show.
    const auto again = engine.evaluate(r.project);
    CHECK(again.volumes[0].status == VolumeStatus::Failed);
  }
}

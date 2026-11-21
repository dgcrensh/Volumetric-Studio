// H5: memory and cancellation in the procedural pipeline. The node cache
// is bounded by bytes, every meshing stage stops on a cancelled token, and
// a job cancelled part way through meshing leaves nothing cached.

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/mesh/tessellate.hpp"
#include "vs/proc/evaluate.hpp"
#include "vs/proc/field.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/mesher.hpp"

using namespace vs;
using namespace vs::proc;
using core::dvec3;
using model::VolumeStatus;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

// Sanitizers slow everything down several times over.
#if defined(__SANITIZE_ADDRESS__)
constexpr double kSlowdown = 5.0;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr double kSlowdown = 5.0;
#else
constexpr double kSlowdown = 1.0;
#endif
#else
constexpr double kSlowdown = 1.0;
#endif

std::vector<Value> list_of(std::size_t doubles) { return {List(doubles, 1.0)}; }

std::size_t bytes_of(const std::vector<Value>& values) {
  std::size_t n = 0;
  for (const auto& v : values) n += value_bytes(v);
  return n;
}

Hash256 key(int i) { return core::hash_of(std::string_view("h5 key"), static_cast<std::uint64_t>(i)); }

// A box boundary's distance, and a gyroid sampled over it.
struct Scene {
  std::unique_ptr<TriangleDistance> boundary;
  SampledField samples;
};

Scene gyroid_scene(double size, double cell) {
  Scene s;
  const auto box = core::dbox3::from_min_max({0, 0, 0}, {size, size, size});
  const auto solid = kernel::make_box(box, kSettings);
  REQUIRE(solid);
  const auto vm = mesh::tessellate(*solid, {core::SubjectKind::Volume, core::Id::random(), "b"}, {}, kSettings, {});
  REQUIRE(vm.ok());
  s.boundary = std::make_unique<TriangleDistance>(vm.mesh, true);
  const auto grid = grid_covering(box, cell);
  REQUIRE(grid);
  eval::ThreadPool pool(0);
  s.samples = sample(*tpms_field(TpmsSurface::Gyroid, 8.0, dvec3{0, 0, 0}, 0.8), *grid, &pool);
  return s;
}

// Declines the work but cancels `token` when offered the unmerged net:
// the cancellation then arrives between the clip and the merge, exactly.
struct CancelAtNet final : MeshingBackend {
  eval::CancellationToken token;
  std::atomic<int> nets{0};
  bool clip(const SampledField&, const TriangleDistance&, double, std::vector<float>&) override { return false; }
  bool unmerged_net(const SampledField&, UnmergedNet&) override {
    ++nets;
    token.cancel();
    return false;
  }
};

struct BackendGuard {
  explicit BackendGuard(std::shared_ptr<MeshingBackend> b) { set_meshing_backend(std::move(b)); }
  ~BackendGuard() { set_meshing_backend(nullptr); }
};

}  // namespace

TEST_CASE("The node cache forgets the least recently used outputs past its byte budget (H5)",
          "[proc][cache]") {
  const std::size_t one = bytes_of(list_of(1000));
  NodeCache cache(3 * one + one / 2);  // room for three
  for (int i = 0; i < 3; ++i) cache.insert(key(i), list_of(1000));
  CHECK(cache.size() == 3);
  CHECK(cache.bytes() == 3 * one);
  REQUIRE(cache.find(key(0)));  // 0 is now the most recently used
  cache.insert(key(3), list_of(1000));
  CHECK(cache.size() == 3);
  CHECK(cache.bytes() <= cache.budget());
  CHECK(cache.find(key(0)));
  CHECK_FALSE(cache.find(key(1)));  // the least recently used went first
  CHECK(cache.find(key(2)));
  CHECK(cache.find(key(3)));

  // One entry larger than the whole budget still stays, alone.
  cache.insert(key(4), list_of(100000));
  CHECK(cache.size() == 1);
  CHECK(cache.find(key(4)));

  // Replacing an entry counts its new size, not both.
  cache.clear();
  cache.insert(key(5), list_of(1000));
  cache.insert(key(5), list_of(1000));
  CHECK(cache.bytes() == one);
}

TEST_CASE("Value sizes count a mesh's arrays and a shared field once (H5)", "[proc][cache]") {
  auto m = std::make_shared<mesh::Mesh>();
  for (int i = 0; i < 100; ++i) m->add_vertex({static_cast<float>(i), 0, 0});
  for (std::uint32_t i = 0; i + 2 < 100; ++i) m->add_triangle(i, i + 1, i + 2, core::Id::random());
  CHECK(value_bytes(Value(MeshValue(m))) >= m->memory_bytes());
  CHECK(m->memory_bytes() >= 100 * sizeof(core::fvec3) + 98 * (3 * sizeof(std::uint32_t) + sizeof(core::Id)));

  const Field ball = sphere_field({0, 0, 0}, 1);
  const std::size_t single = value_bytes(Value(ball));
  // The same subtree twice: counted once, plus the union's own node.
  const std::size_t twice = value_bytes(Value(union_field(ball, ball)));
  CHECK(twice > single);
  CHECK(twice < 2 * single);
}

TEST_CASE("Every meshing stage stops at once on a cancelled token (H5)", "[proc][mesher][cancel]") {
  const Scene s = gyroid_scene(16, 0.4);
  eval::CancellationToken cancelled;
  cancelled.cancel();
  eval::ThreadPool pool(4);
  CHECK(clip_to_boundary(s.samples, *s.boundary, 0.0, &pool, &cancelled).values.empty());
  CHECK(clip_to_boundary_cpu(s.samples, *s.boundary, 0.0, nullptr, &cancelled).values.empty());
  CHECK(unmerged_net(s.samples, &pool, &cancelled).triangles.empty());
  CHECK(unmerged_net_cpu(s.samples, nullptr, &cancelled).triangles.empty());
  CHECK(surface_nets(s.samples, &pool, &cancelled).empty());
  UnmergedNet net = unmerged_net(s.samples, &pool);
  REQUIRE_FALSE(net.triangles.empty());
  CHECK(merge_net(net, s.samples.grid, &pool, &cancelled).empty());
  CHECK(merge_net(net, s.samples.grid, nullptr, &cancelled).empty());
  const core::SubjectRef volume{core::SubjectKind::Volume, core::Id::random(), "V"};
  const ContentsMesh cm = mesh_contents(s.samples, *s.boundary, {}, volume, &pool, &cancelled);
  CHECK(cm.cancelled);
  CHECK(cm.mesh.empty());
  CHECK(cm.diagnostics.empty());

  // A token never cancelled changes nothing: the same mesh as without one.
  const eval::CancellationToken idle;
  CHECK(mesh_contents(s.samples, *s.boundary, {}, volume, &pool, &idle).mesh ==
        mesh_contents(s.samples, *s.boundary, {}, volume, &pool).mesh);
}

TEST_CASE("Cancelling mid-mesh_contents returns within 250 ms (H5)", "[proc][mesher][cancel]") {
  const Scene s = gyroid_scene(40, 0.3);
  const core::SubjectRef volume{core::SubjectKind::Volume, core::Id::random(), "V"};
  eval::ThreadPool pool(4);
  using Clock = std::chrono::steady_clock;

  // How long it takes uncancelled; the cancellation lands a fifth of the
  // way in (in the clip or the nets, the slowest stages).
  auto t0 = Clock::now();
  const ContentsMesh whole = mesh_contents(s.samples, *s.boundary, {}, volume, &pool);
  const auto full = Clock::now() - t0;
  REQUIRE_FALSE(whole.cancelled);
  REQUIRE_FALSE(whole.mesh.empty());
  const double full_ms = std::chrono::duration<double, std::milli>(full).count();
  INFO("uncancelled: " << full_ms << " ms");
  REQUIRE(full > std::chrono::milliseconds(100));

  for (const double at : {0.2, 0.5, 0.8}) {
    eval::CancellationToken token;
    std::atomic<bool> done{false};
    ContentsMesh cm;
    Clock::time_point returned;
    std::thread worker([&] {
      cm = mesh_contents(s.samples, *s.boundary, {}, volume, &pool, &token);
      returned = Clock::now();
      done = true;
    });
    std::this_thread::sleep_for(std::chrono::duration_cast<Clock::duration>(full * at));
    const auto cancelled_at = Clock::now();
    const bool finished_first = done.load();
    token.cancel();
    worker.join();
    if (finished_first) continue;  // a fast machine: nothing to time
    const double latency = std::chrono::duration<double, std::milli>(returned - cancelled_at).count();
    INFO("cancelled at " << at << " of the way; returned " << latency << " ms later");
    CHECK(cm.cancelled);
    CHECK(cm.mesh.empty());
    CHECK(latency < 250.0 * kSlowdown);
  }
}

TEST_CASE("A job cancelled during meshing is dropped, not cached (H5)", "[proc][engine][cancel]") {
  const auto s = scenes::shell_and_ball();
  auto b = scenes::boxes({s.graph});
  eval::EngineOptions eo;
  eo.threads = 2;
  eval::Engine engine(kSettings, eo);
  install(engine);

  // The backend cancels as the clip ends and the nets begin: sampling has
  // finished, meshing has started.
  auto backend = std::make_shared<CancelAtNet>();
  {
    const BackendGuard guard(backend);
    eval::EvaluateOptions options;
    options.cancel = backend->token;
    const auto ev = engine.evaluate(b.project, options);
    REQUIRE(backend->nets >= 1);
    CHECK(ev.cancelled);
    CHECK(ev.volumes[0].status == VolumeStatus::Stale);
    CHECK_FALSE(ev.volumes[0].contents);
    CHECK(engine.cache().memory_size() == 0);
  }
  // Nothing of it was kept: the next evaluation generates it whole.
  const auto again = engine.evaluate(b.project);
  REQUIRE(again.ok());
  CHECK(again.volumes[0].regenerated);
  CHECK(engine.cache().memory_size() == 1);
}

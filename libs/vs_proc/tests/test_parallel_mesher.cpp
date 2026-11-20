// Parallel surface nets and the meshing backend hook (L1b): the mesh is the
// serial reference's, bit for bit, on any number of threads.

#include <memory>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "vs/eval/thread_pool.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/mesh/tessellate.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/proc/mesher.hpp"

using namespace vs;
using namespace vs::proc;
using core::dvec3;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

SampledField random_grid(std::mt19937_64& rng, std::uint32_t nx, std::uint32_t ny, std::uint32_t nz,
                         int kind) {
  SampledField s;
  s.grid.origin = {-1.25, 0.5, 3.0};
  s.grid.cell = 0.75;
  s.grid.nx = nx;
  s.grid.ny = ny;
  s.grid.nz = nz;
  s.values.resize(s.grid.size());
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  if (kind == 2) {
    // Smooth: a coarse random lattice, interpolated, gives thin walls,
    // tunnels and touching blobs rather than noise.
    const std::uint32_t c = 4;
    const std::uint32_t mx = nx / c + 2;
    const std::uint32_t my = ny / c + 2;
    const std::uint32_t mz = nz / c + 2;
    std::vector<float> coarse(static_cast<std::size_t>(mx) * my * mz);
    for (float& v : coarse) v = u(rng);
    const auto at = [&](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
      return coarse[(static_cast<std::size_t>(k) * my + j) * mx + i];
    };
    for (std::uint32_t k = 0; k < nz; ++k) {
      for (std::uint32_t j = 0; j < ny; ++j) {
        for (std::uint32_t i = 0; i < nx; ++i) {
          const float fx = static_cast<float>(i % c) / static_cast<float>(c);
          const float fy = static_cast<float>(j % c) / static_cast<float>(c);
          const float fz = static_cast<float>(k % c) / static_cast<float>(c);
          const std::uint32_t I = i / c;
          const std::uint32_t J = j / c;
          const std::uint32_t K = k / c;
          float v = 0.0f;
          for (std::uint32_t d = 0; d < 8; ++d) {
            const float w = ((d & 1) ? fx : 1 - fx) * ((d & 2) ? fy : 1 - fy) * ((d & 4) ? fz : 1 - fz);
            v += w * at(I + (d & 1u), J + ((d >> 1) & 1u), K + ((d >> 2) & 1u));
          }
          s.values[s.grid.index(i, j, k)] = v;
        }
      }
    }
  } else {
    for (float& v : s.values) v = u(rng);
  }
  if (kind == 1) {
    // Exact zeros (outside) and touching corners.
    for (std::size_t i = 0; i < s.values.size(); i += 3) s.values[i] = 0.0f;
  }
  return s;
}

void require_same(const FieldMesh& a, const FieldMesh& b) {
  REQUIRE(a.positions.size() == b.positions.size());
  REQUIRE(a.triangles.size() == b.triangles.size());
  REQUIRE(a.positions == b.positions);
  REQUIRE(a.triangles == b.triangles);
}

}  // namespace

TEST_CASE("Parallel surface nets give the serial reference's mesh on small grids",
          "[proc][mesher][parallel]") {
  std::mt19937_64 rng(1919);
  std::uniform_int_distribution<std::uint32_t> size(1, 11);
  eval::ThreadPool one(1);
  eval::ThreadPool three(3);
  eval::ThreadPool many(12);
  for (int round = 0; round < 3000; ++round) {
    const SampledField s = random_grid(rng, size(rng), size(rng), size(rng), round % 3);
    INFO("round " << round);
    const FieldMesh ref = surface_nets_reference(s);
    require_same(surface_nets(s), ref);
    require_same(surface_nets(s, &one), ref);
    require_same(surface_nets(s, &three), ref);
    require_same(surface_nets(s, &many), ref);
  }
}

TEST_CASE("Parallel surface nets give the serial reference's mesh when waves run in parallel",
          "[proc][mesher][parallel]") {
  // Grids big enough that waves hold many chunks of cubes, so neighbouring
  // waves' cubes really do run on different threads.
  std::mt19937_64 rng(2026);
  eval::ThreadPool two(2);
  eval::ThreadPool three(3);
  eval::ThreadPool many(12);
  for (int round = 0; round < 9; ++round) {
    const std::uint32_t n = 40 + static_cast<std::uint32_t>(round) * 3;
    const SampledField s = random_grid(rng, n, n - 5, n + 4, round % 3);
    INFO("round " << round);
    const FieldMesh ref = surface_nets_reference(s);
    REQUIRE_FALSE(ref.empty());
    const UnmergedNet net = unmerged_net_cpu(s);
    CHECK(unmerged_net_cpu(s, &many) == net);
    require_same(merge_net(net, s.grid, &two), ref);
    require_same(surface_nets(s, &three), ref);
    require_same(surface_nets(s, &many), ref);
  }
}

TEST_CASE("Parallel surface nets give the serial reference's mesh on a gyroid",
          "[proc][mesher][parallel]") {
  const auto f = tpms_field(TpmsSurface::Gyroid, 6.0, dvec3{0, 0, 0}, 0.6);
  const auto grid = grid_covering(core::dbox3::from_min_max({0, 0, 0}, {30, 24, 20}), 0.3);
  REQUIRE(grid);
  const SampledField s = sample(*f, *grid);
  const FieldMesh ref = surface_nets_reference(s);
  eval::ThreadPool many(12);
  require_same(surface_nets(s, &many), ref);
}

namespace {

// Declines everything, counting what it was offered.
struct Declining final : MeshingBackend {
  int clips = 0;
  int nets = 0;
  bool clip(const SampledField&, const TriangleDistance&, double, std::vector<float>&) override {
    ++clips;
    return false;
  }
  bool unmerged_net(const SampledField&, UnmergedNet&) override {
    ++nets;
    return false;
  }
};

// Takes everything, doing it the CPU's way.
struct Taking final : MeshingBackend {
  int clips = 0;
  int nets = 0;
  bool clip(const SampledField& s, const TriangleDistance& b, double inset,
            std::vector<float>& values) override {
    ++clips;
    values = clip_to_boundary_cpu(s, b, inset).values;
    return true;
  }
  bool unmerged_net(const SampledField& s, UnmergedNet& net) override {
    ++nets;
    net = unmerged_net_cpu(s);
    return true;
  }
};

}  // namespace

TEST_CASE("The meshing backend is offered the clip and the unmerged net", "[proc][mesher][parallel]") {
  const auto f = smooth_union_field(sphere_field({0, 0, 0}, 5), box_field({4, 1, 0}, {6, 3, 8}), 1.5);
  const auto grid = grid_covering(core::dbox3::from_min_max({-8, -8, -8}, {10, 8, 8}), 0.4);
  REQUIRE(grid);
  const SampledField s = sample(*f, *grid);
  const auto solid = kernel::make_box(core::dbox3::from_min_max({-6, -6, -6}, {9, 6, 6}), kSettings);
  REQUIRE(solid);
  const auto vm = mesh::tessellate(*solid, {core::SubjectKind::Volume, core::Id::random(), "b"}, {},
                                   kSettings, {});
  REQUIRE(vm.ok());
  const TriangleDistance boundary(vm.mesh, true);
  const core::SubjectRef volume{core::SubjectKind::Volume, core::Id::random(), "V"};
  eval::ThreadPool pool(4);
  const ContentsMesh expected = mesh_contents(s, boundary, {}, volume, &pool);

  auto declining = std::make_shared<Declining>();
  set_meshing_backend(declining);
  const ContentsMesh a = mesh_contents(s, boundary, {}, volume, &pool);
  auto taking = std::make_shared<Taking>();
  set_meshing_backend(taking);
  const ContentsMesh b = mesh_contents(s, boundary, {}, volume, &pool);
  set_meshing_backend(nullptr);
  CHECK(meshing_backend() == nullptr);

  CHECK(declining->clips == 1);
  CHECK(declining->nets == 1);
  CHECK(taking->clips == 1);
  CHECK(taking->nets == 1);
  CHECK(a.mesh == expected.mesh);
  CHECK(b.mesh == expected.mesh);
}

TEST_CASE("The parallel closed-manifold check never passes what analyse rejects",
          "[proc][mesher][parallel]") {
  eval::ThreadPool pool(4);
  const auto agrees = [&](const mesh::Mesh& m) {
    const bool fast = closed_manifold(m, &pool);
    const auto t = mesh::analyse(m);
    const bool full = t.watertight() && t.manifold();
    if (fast) REQUIRE(full);
    return fast;
  };
  const auto grid = grid_covering(core::dbox3::from_min_max({-6, -6, -6}, {6, 6, 6}), 0.7);
  REQUIRE(grid);
  const mesh::Mesh ball = to_mesh(surface_nets(sample(*sphere_field({0, 0, 0}, 4.0), *grid)), core::Id::random());
  CHECK(agrees(ball));

  // Two tetrahedra sharing a vertex: closed, every edge fine, but pinched.
  mesh::Mesh pinched;
  for (const core::fvec3 p : {core::fvec3{0, 0, 0}, core::fvec3{1, 0, 0}, core::fvec3{0, 1, 0},
                              core::fvec3{0, 0, 1}, core::fvec3{-1, 0, 0}, core::fvec3{0, -1, 0},
                              core::fvec3{0, 0, -1}}) {
    pinched.add_vertex(p);
  }
  const core::Id id = core::Id::random();
  for (const mesh::Triangle t : {mesh::Triangle{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}, {0, 5, 4},
                                 {0, 4, 6}, {0, 6, 5}, {4, 5, 6}}) {
    pinched.add_triangle(t, id);
  }
  CHECK_FALSE(agrees(pinched));

  std::mt19937_64 rng(5);
  for (int round = 0; round < 200; ++round) {
    mesh::Mesh m = ball;
    std::uniform_int_distribution<std::size_t> pick(0, m.triangle_count() - 1);
    const std::size_t t = pick(rng);
    switch (round % 5) {
      case 0:  // a hole
        m.indices.erase(m.indices.begin() + static_cast<std::ptrdiff_t>(3 * t),
                        m.indices.begin() + static_cast<std::ptrdiff_t>(3 * t + 3));
        m.triangle_volumes.pop_back();
        break;
      case 1:  // flipped
        std::swap(m.indices[3 * t], m.indices[3 * t + 1]);
        break;
      case 2:  // doubled
        m.add_triangle(m.triangle(t), id);
        break;
      case 3:  // degenerate
        m.indices[3 * t + 1] = m.indices[3 * t];
        break;
      default:  // a corner moved to another vertex
        m.indices[3 * t + 2] = m.indices[3 * pick(rng)];
        break;
    }
    INFO("round " << round);
    CHECK_FALSE(agrees(m));
  }
}

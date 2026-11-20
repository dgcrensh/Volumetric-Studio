// GPU meshing (L1b): the clip and the unmerged surface net match the CPU's
// element for element, and the calibration routes each stage.

#include <chrono>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "vs/eval/thread_pool.hpp"
#include "vs/gpu/gpu.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/mesh/tessellate.hpp"
#include "vs/proc/mesher.hpp"

using namespace vs;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

bool have_device() {
  std::string error;
  const auto grid = proc::grid_covering(core::dbox3::from_min_max({0, 0, 0}, {1, 1, 1}), 0.5);
  if (!gpu::sample_gpu(*proc::constant_field(1.0), *grid, &error)) {
    WARN("no GPU: " << error);
    return false;
  }
  return true;
}

}  // namespace

TEST_CASE("The GPU's clip and unmerged nets are the CPU's", "[gpu][meshing][acceptance]") {
  if (!have_device()) return;
  const std::string mismatch = gpu::meshing_self_check();
  INFO(mismatch);
  CHECK(mismatch.empty());
}

TEST_CASE("A gyroid in a cylinder meshes the same on the GPU", "[gpu][meshing]") {
  if (!have_device()) return;
  const auto solid = kernel::make_cylinder(kernel::Axis{{0, 0, -12}, {0, 0, 1}}, 14.0, 24.0, kSettings);
  REQUIRE(solid);
  const auto vm = mesh::tessellate(*solid, {core::SubjectKind::Volume, core::Id::random(), "c"}, {},
                                   kSettings, {});
  REQUIRE(vm.ok());
  const proc::TriangleDistance boundary(vm.mesh, true);
  const auto field = proc::tpms_field(proc::TpmsSurface::Gyroid, 5.0, {0, 0, 0}, 0.7);
  const auto grid = proc::grid_covering(core::dbox3::from_min_max({-15, -15, -13}, {15, 15, 13}), 0.3);
  REQUIRE(grid);
  const proc::SampledField samples = proc::sample_cpu(*field, *grid);

  const auto t0 = std::chrono::steady_clock::now();
  const auto clipped = gpu::clip_gpu(samples, boundary, proc::kContentsInset);
  const auto t1 = std::chrono::steady_clock::now();
  REQUIRE(clipped);
  const proc::SampledField cpu_clip = proc::clip_to_boundary_cpu(samples, boundary, proc::kContentsInset);
  CHECK(*clipped == cpu_clip.values);

  const auto t2 = std::chrono::steady_clock::now();
  std::string error;
  const auto net = gpu::unmerged_net_gpu(cpu_clip, &error);
  const auto t3 = std::chrono::steady_clock::now();
  INFO(error);
  REQUIRE(net);
  CHECK(*net == proc::unmerged_net_cpu(cpu_clip));
  const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  WARN(fmt::format("{} samples: clip {:.0f} ms, unmerged net {:.0f} ms ({} triangles) on the GPU", grid->size(),
                   ms(t0, t1), ms(t2, t3), net->triangles.size()));
}

TEST_CASE("Installed with force, mesh_contents uses the GPU and gives the CPU's mesh", "[gpu][meshing]") {
  if (!have_device()) return;
  const auto field = proc::smooth_union_field(proc::sphere_field({0, 0, 0}, 9),
                                              proc::box_field({6, 1, 0}, {8, 4, 12}), 2.0);
  const auto grid = proc::grid_covering(core::dbox3::from_min_max({-12, -12, -12}, {16, 12, 14}), 0.25);
  REQUIRE(grid);
  const proc::SampledField s = proc::sample_cpu(*field, *grid);
  const auto solid = kernel::make_box(core::dbox3::from_min_max({-10, -10, -10}, {13, 10, 11}), kSettings);
  REQUIRE(solid);
  const auto vm = mesh::tessellate(*solid, {core::SubjectKind::Volume, core::Id::random(), "b"}, {},
                                   kSettings, {});
  REQUIRE(vm.ok());
  const proc::TriangleDistance boundary(vm.mesh, true);
  const core::SubjectRef volume{core::SubjectKind::Volume, core::Id::random(), "V"};
  eval::ThreadPool pool(4);
  const proc::ContentsMesh cpu = proc::mesh_contents(s, boundary, {}, volume, &pool);

  gpu::InstallOptions o;
  o.force = true;
  const gpu::Status status = gpu::install(o);
  INFO(status.why);
  REQUIRE(status.installed);
  CHECK(status.sampling);
  CHECK(status.clip);
  CHECK(status.meshing);
  REQUIRE(proc::meshing_backend() != nullptr);
  const proc::ContentsMesh viagpu = proc::mesh_contents(s, boundary, {}, volume, &pool);
  gpu::uninstall();
  CHECK(proc::meshing_backend() == nullptr);
  CHECK(viagpu.mesh == cpu.mesh);
}

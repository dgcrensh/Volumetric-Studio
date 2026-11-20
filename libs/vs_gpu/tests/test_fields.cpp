// L1 acceptance: fields sampled on the GPU are the CPU's samples, bit for
// bit, operation by operation and through the engine. Skipped (with a
// warning) where there is no GPU device.

#include <chrono>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "glsl.hpp"
#include "vs/eval/engine.hpp"
#include "vs/gpu/gpu.hpp"
#include "vs/model/project.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/templates.hpp"

using namespace vs;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

bool have_gpu() {
  std::string error;
  const auto field = proc::constant_field(1.0);
  const auto grid = proc::grid_covering(core::dbox3::from_min_max({0, 0, 0}, {1, 1, 1}), 0.5);
  if (!gpu::sample_gpu(*field, *grid, &error)) {
    WARN("No GPU sampling here: " << error);
    return false;
  }
  return true;
}

// Uninstalls on the way out, so other tests sample on the CPU.
struct Installed {
  gpu::Status status;
  explicit Installed(std::size_t min_samples = 0) {
    gpu::InstallOptions o;
    o.min_samples = min_samples;
    status = gpu::install(o);
  }
  ~Installed() { gpu::uninstall(); }
};

}  // namespace

TEST_CASE("Every field operation samples to the CPU's values on the GPU", "[gpu][fields][acceptance]") {
  if (!have_gpu()) return;
  INFO(gpu::status().device);
  const std::string mismatch = gpu::self_check();
  INFO(mismatch);
  CHECK(mismatch.empty());
}

TEST_CASE("Field shaders depend on a tree's shape, not its numbers", "[gpu][fields]") {
  const auto a = gpu::translate(*proc::sphere_field({1, 2, 3}, 4.0));
  const auto b = gpu::translate(*proc::sphere_field({5, 6, 7}, 8.0));
  CHECK(a.source == b.source);
  CHECK(a.d != b.d);
  const auto c = gpu::translate(*proc::box_field({1, 2, 3}, {4, 4, 4}));
  CHECK(c.source != a.source);
  // Literals are exact bit patterns.
  CHECK(gpu::glsl_double(0.1) == "packDouble2x32(uvec2(0x9999999au, 0x3fb99999u))");
}

TEST_CASE("Installed, sample() sends grids to the GPU and gives the CPU's hash", "[gpu][fields]") {
  if (!have_gpu()) return;
  const Installed installed;
  INFO(installed.status.why);
  REQUIRE(installed.status.installed);
  REQUIRE(proc::sampling_backend() != nullptr);
  const auto field = proc::tpms_field(proc::TpmsSurface::Gyroid, 5.0, {0, 0, 0}, 0.7);
  const auto grid = proc::grid_covering(core::dbox3::from_min_max({0, 0, 0}, {40, 30, 20}), 0.25);
  REQUIRE(grid.ok());
  const auto t0 = std::chrono::steady_clock::now();
  const auto viagpu = proc::sample(*field, *grid);
  const auto t1 = std::chrono::steady_clock::now();
  const auto cpu = proc::sample_cpu(*field, *grid);
  const auto t2 = std::chrono::steady_clock::now();
  CHECK(viagpu.hash() == cpu.hash());
  WARN(fmt::format("{} samples: GPU {:.0f} ms, CPU (one thread) {:.0f} ms", grid->size(),
                   std::chrono::duration<double, std::milli>(t1 - t0).count(),
                   std::chrono::duration<double, std::milli>(t2 - t1).count()));

  SECTION("a cancelled token stops it") {
    eval::CancellationToken cancel;
    cancel.cancel();
    bool stopped = false;
    proc::sample(*field, *grid, nullptr, &cancel, &stopped);
    CHECK(stopped);
  }
}

TEST_CASE("Procedural projects evaluate to the same hashes with the GPU", "[gpu][fields][acceptance]") {
  if (!have_gpu()) return;
  const std::filesystem::path data(VS_PROC_TEST_DATA);
  for (const char* name : {"shell_and_ball.vsproj", "bolt_bosses.vsproj"}) {
    INFO(name);
    auto project = model::load_project(data / name);
    REQUIRE(project.ok());
    const auto evaluate = [&] {
      eval::Engine engine(kSettings, {.threads = 4});
      proc::install(engine);
      return engine.evaluate(*project);
    };
    const auto on_cpu = evaluate();
    REQUIRE(on_cpu.ok());
    core::Hash256 on_gpu_hash;
    {
      const Installed installed;
      INFO(installed.status.why);
  REQUIRE(installed.status.installed);
      const auto on_gpu = evaluate();
      REQUIRE(on_gpu.ok());
      on_gpu_hash = on_gpu.output_hash();
    }
    CHECK(on_gpu_hash == on_cpu.output_hash());
  }
}

TEST_CASE("The M12 templates sample the same on the GPU", "[gpu][fields][acceptance]") {
  if (!have_gpu()) return;
  // Each template in a box volume next to a bolted neighbour, through the
  // engine, so the boundary distances and seams take part.
  for (const auto& info : proc::templates()) {
    INFO(info.name);
    std::string error;
    // The template's graph alone, with the shell_and_ball project's box.
    auto project = model::load_project(std::filesystem::path(VS_PROC_TEST_DATA) / "shell_and_ball.vsproj");
    REQUIRE(project.ok());
    if (info.needs_surface) continue;  // bolt_bosses is covered by its sample project
    proc::TemplateOptions opts;
    opts.cell_size = 0.5;
    const auto graph = proc::make_template(info.name, opts);
    REQUIRE(graph.ok());
    auto& v = project->document.volumes.front();
    v.contents = model::ProceduralContents{proc::to_json(*graph)};
    const auto evaluate = [&] {
      eval::Engine engine(kSettings, {.threads = 4});
      proc::install(engine);
      return engine.evaluate(*project);
    };
    const auto on_cpu = evaluate();
    INFO((on_cpu.diagnostics.items().empty() ? std::string() : on_cpu.diagnostics.items().front().message));
    REQUIRE(on_cpu.ok());
    core::Hash256 on_gpu_hash;
    {
      const Installed installed;
      const auto on_gpu = evaluate();
      REQUIRE(on_gpu.ok());
      on_gpu_hash = on_gpu.output_hash();
    }
    CHECK(on_gpu_hash == on_cpu.output_hash());
  }
}

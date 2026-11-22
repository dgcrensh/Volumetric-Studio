// H6: the device thread, GL safety and calibration. Tests that need a GPU
// skip (with a warning) where there is none.

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "device.hpp"
#include "glsl.hpp"
#include "state.hpp"
#include "vs/gpu/gpu.hpp"
#include "vs/proc/distance.hpp"

using namespace vs;
using namespace vs::gpu;

namespace {

const char* kTrivial = R"(#version 430
layout(local_size_x = 64) in;
layout(std430, binding = 0) writeonly buffer O { uint o[]; };
uniform uint n;
void main() {
  uint g = gl_GlobalInvocationID.y * (gl_NumWorkGroups.x * gl_WorkGroupSize.x) + gl_GlobalInvocationID.x;
  if (g < n) o[g] = g * 3u + VALUE;
}
)";

std::string trivial(int value) {
  std::string s = kTrivial;
  s.replace(s.find("VALUE"), 5, fmt::format("{}u", value));
  return s;
}

mesh::Mesh box_mesh(core::fvec3 lo, core::fvec3 hi) {
  mesh::Mesh m;
  for (int i = 0; i < 8; ++i) {
    m.add_vertex({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
  }
  const core::Id v = core::Id::random();
  const std::uint32_t quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                     {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (const auto& q : quads) {
    m.add_triangle(q[0], q[1], q[2], v);
    m.add_triangle(q[0], q[2], q[3], v);
  }
  return m;
}

bool same_bits(const std::vector<float>& a, const std::vector<float>& b, std::size_t& first) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::bit_cast<std::uint32_t>(a[i]) != std::bit_cast<std::uint32_t>(b[i])) {
      first = i;
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("Two-dimensional dispatches stay within 65,535 groups a side", "[gpu][device]") {
  CHECK(dispatch_2d(0).x == 1);
  CHECK(dispatch_2d(0).y == 1);
  CHECK(dispatch_2d(7).x == 7);
  CHECK(dispatch_2d(7).y == 1);
  CHECK(dispatch_2d(65535).y == 1);
  const Dispatch d = dispatch_2d(65536);
  CHECK(d.x == 65535);
  CHECK(d.y == 2);
  for (const std::size_t groups : {std::size_t{1} << 20, std::size_t{123456789}, std::size_t{65535} * 65535}) {
    const Dispatch e = dispatch_2d(groups);
    CHECK(e.x <= 65535);
    CHECK(e.y <= 65535);
    CHECK(std::size_t{e.x} * e.y >= groups);
    CHECK(std::size_t{e.x} * (e.y - 1) < groups);
  }
}

TEST_CASE("A GPU stage that failed never wins the calibration", "[gpu][device]") {
  CHECK(gpu_wins(10.0, 20.0));
  CHECK_FALSE(gpu_wins(20.0, 10.0));
  CHECK_FALSE(gpu_wins(std::numeric_limits<double>::infinity(), 1e300));
  CHECK_FALSE(gpu_wins(std::numeric_limits<double>::quiet_NaN(), 10.0));
}

TEST_CASE("A field with many boundary meshes defines every mesh slot", "[gpu][device]") {
  // Twelve distinct meshes: MESH1 is a prefix of MESH10 and MESH11, which
  // the old search by name could confuse.
  std::vector<std::shared_ptr<const proc::TriangleDistance>> meshes;
  proc::Field field = proc::constant_field(100.0);
  for (int i = 0; i < 12; ++i) {
    const float x = 3.0f * static_cast<float>(i);
    meshes.push_back(std::make_shared<const proc::TriangleDistance>(box_mesh({x, 0, 0}, {x + 1, 1, 1}), true));
    field = proc::union_field(field, proc::mesh_signed_field(meshes.back()));
  }
  const FieldProgram p = translate(*field);
  CHECK(p.meshes == 12);
  for (int m = 0; m < 12; ++m) CHECK(p.source.find(fmt::format("#define MESH{} ", m)) != std::string::npos);
  CHECK(p.source.find("#define MESH12 ") == std::string::npos);

  const auto grid = proc::grid_covering(core::dbox3::from_min_max({-1, -1, -1}, {36, 2, 2}), 0.25);
  REQUIRE(grid);
  std::string error;
  const auto gpu = sample_gpu(*field, *grid, &error);
  if (!gpu) {
    WARN("No GPU sampling here: " << error);
    return;
  }
  const auto cpu = proc::sample_cpu(*field, *grid);
  std::size_t first = 0;
  INFO(first);
  CHECK(same_bits(gpu->values, cpu.values, first));
}

TEST_CASE("The device thread survives many callers, throwing jobs and small buffers", "[gpu][device]") {
  Device device;
  if (!device.ok()) {
    WARN("No GPU device: " << device.why());
    return;
  }
  INFO(device.description());
  CHECK(device.max_block_bytes() >= (std::size_t{1} << 24));

  SECTION("callers on many threads each get their own job's result") {
    // Before H6 the job notified a condition variable on the caller's
    // stack after releasing its mutex: a caller woken early returned and
    // freed it while the device thread still used it.
    constexpr int kThreads = 8;
    constexpr int kJobs = 2000;
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        for (int j = 0; j < kJobs; ++j) {
          int seen = -1;
          const bool ran = device.run([&](const gl::Functions&) { seen = t * kJobs + j; });
          if (!ran || seen != t * kJobs + j) ++wrong;
        }
      });
    }
    for (auto& th : threads) th.join();
    CHECK(wrong.load() == 0);
  }

  SECTION("a throwing job reaches its caller, and the device carries on") {
    CHECK_THROWS_AS(device.run([](const gl::Functions&) { throw std::runtime_error("job failed"); }),
                    std::runtime_error);
    bool ran = false;
    CHECK(device.run([&](const gl::Functions&) { ran = true; }));
    CHECK(ran);
  }

  SECTION("a buffer shorter than the 16-byte minimum keeps its data and zeros the rest") {
    const std::uint32_t data = 0xA1B2C3D4u;
    std::uint32_t back[4] = {1, 1, 1, 1};
    device.run([&](const gl::Functions& f) {
      Buffer b(f, sizeof(data), &data);
      b.read(back, sizeof(back));
    });
    CHECK(back[0] == data);
    CHECK(back[1] == 0);
    CHECK(back[2] == 0);
    CHECK(back[3] == 0);
  }

  SECTION("GL errors are reported, not swallowed") {
    std::string errors;
    device.run([&](const gl::Functions& f) {
      f.BindBuffer(0x1234, 0);  // not a buffer target: GL_INVALID_ENUM
      errors = gl_errors(f);
    });
    CHECK(errors == "0x0500");
    device.run([&](const gl::Functions& f) { errors = gl_errors(f); });
    CHECK(errors.empty());
  }

  SECTION("programs are capped, least recently used first, and come back on demand") {
    std::string log;
    gl::GLuint first = 0;
    device.run([&](const gl::Functions& f) { first = device.program(f, trivial(0), log); });
    REQUIRE(first != 0);
    INFO(log);
    for (int i = 1; i <= static_cast<int>(Device::kMaxPrograms) + 6; ++i) {
      device.run([&](const gl::Functions& f) {
        // Keep program 0 in use: it must survive every eviction.
        CHECK(device.program(f, trivial(0), log) == first);
        CHECK(device.program(f, trivial(i), log) != 0);
      });
    }
    CHECK(device.program_count() == Device::kMaxPrograms);
    // Program 1 went first; it compiles again and runs.
    std::vector<std::uint32_t> out(100);
    device.run([&](const gl::Functions& f) {
      const gl::GLuint id = device.program(f, trivial(1), log);
      REQUIRE(id != 0);
      Buffer o(f, out.size() * sizeof(std::uint32_t));
      o.bind(0);
      f.UseProgram(id);
      f.Uniform1ui(f.GetUniformLocation(id, "n"), static_cast<gl::GLuint>(out.size()));
      f.DispatchCompute(2, 1, 1);
      f.MemoryBarrier(gl::kBufferUpdateBarrierBit);
      o.read(out.data(), out.size() * sizeof(std::uint32_t));
    });
    CHECK(device.program_count() == Device::kMaxPrograms);
    for (std::uint32_t i = 0; i < out.size(); ++i) CHECK(out[i] == 3 * i + 1);
  }
}

TEST_CASE("A wide, flat grid samples to the CPU's values on the GPU", "[gpu][device]") {
  // 2,100 x 2,100 samples a slice: one slice needs 68,907 workgroups of 64,
  // past GL's 65,535 a dimension. Before H6 the dispatch was refused and
  // the slice read back as zeros.
  const proc::GridSpec grid{.origin = {-10.5, -10.5, -0.01}, .cell = 0.01, .nx = 2100, .ny = 2100, .nz = 2};
  const auto field = proc::sphere_field({0.3, -0.2, 0}, 7.5);
  std::string error;
  const auto gpu = sample_gpu(*field, grid, &error);
  if (!gpu) {
    WARN("No GPU sampling here: " << error);
    return;
  }
  const auto cpu = proc::sample_cpu(*field, grid);
  std::size_t first = 0;
  const bool same = same_bits(gpu->values, cpu.values, first);
  INFO(fmt::format("first difference at {}: CPU {} GPU {}", first, cpu.values[first], gpu->values[first]));
  CHECK(same);
}

TEST_CASE("Concurrent installs check and calibrate once", "[gpu][device]") {
  {
    std::string why;
    if (shared_device(&why) == nullptr) {
      WARN("No GPU device: " << why);
      return;
    }
  }
  gpu::uninstall();
  // Forced, so no calibration timing: what matters is that both callers
  // see one installed status.
  InstallOptions o;
  o.force = true;
  Status a, b;
  std::thread t1([&] { a = install(o); });
  std::thread t2([&] { b = install(o); });
  t1.join();
  t2.join();
  CHECK(a.installed);
  CHECK(b.installed);
  CHECK(a.install_ms == b.install_ms);
  gpu::uninstall();
}

// Hidden (run by its own CTest entry, in a process of its own): after it
// the device is gone for the rest of the process.
TEST_CASE("Shutdown closes the device for good, even while a call holds it", "[.shutdown]") {
  std::string why;
  std::shared_ptr<Device> held = shared_device(&why);
  if (held == nullptr) {
    WARN("No GPU device: " << why);
    gpu::shutdown();
    CHECK(shared_device(&why) == nullptr);
    return;
  }
  gpu::shutdown();
  // A call already holding the device finishes on it.
  bool ran = false;
  CHECK(held->run([&](const gl::Functions&) { ran = true; }));
  CHECK(ran);
  held.reset();  // the last holder closes it here
  // New calls find none, and say why.
  CHECK(shared_device(&why) == nullptr);
  CHECK(why == "the GPU was shut down");
  std::string error;
  const auto grid = proc::grid_covering(core::dbox3::from_min_max({0, 0, 0}, {1, 1, 1}), 0.5);
  CHECK_FALSE(sample_gpu(*proc::constant_field(1.0), *grid, &error));
  CHECK(error == "the GPU was shut down");
  CHECK_FALSE(install().installed);
  gpu::shutdown();  // twice is fine
}

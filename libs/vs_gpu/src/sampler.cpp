#include "vs/gpu/gpu.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>

#include <fmt/format.h>

#include "device.hpp"
#include "glsl.hpp"
#include "vs/core/transform.hpp"
#include "vs/proc/distance.hpp"

namespace vs::gpu {

namespace {

struct State {
  std::mutex mutex;
  std::unique_ptr<Device> device;
  bool tried = false;
  Status status;
  InstallOptions options;
  std::set<std::string> failed_sources;  // shaders that didn't compile
  std::size_t slab_samples = 1 << 16;    // adapted to keep dispatches short
};

State& state() {
  static State s;
  return s;
}

Device* device_locked(State& s) {
  if (!s.tried) {
    s.tried = true;
    s.device = std::make_unique<Device>();
    if (s.device->ok()) {
      s.status.device = s.device->description();
    } else {
      s.status.why = s.device->why();
    }
  }
  return s.device && s.device->ok() ? s.device.get() : nullptr;
}

class GpuBackend final : public proc::SamplingBackend {
 public:
  bool sample(const proc::FieldExpr& field, const proc::GridSpec& grid, std::vector<float>& values,
              const eval::CancellationToken* cancel, bool* cancelled) override {
    std::size_t min_samples = 0;
    {
      std::lock_guard lock(state().mutex);
      min_samples = state().options.min_samples;
    }
    if (grid.size() < min_samples) return false;
    auto r = sample_gpu(field, grid, nullptr, cancel, cancelled);
    if (!r) return false;
    values = std::move(r->values);
    return true;
  }
};

// Target time per dispatch; Windows resets the GPU after 2 s.
constexpr double kTargetMs = 100.0;

}  // namespace

std::optional<proc::SampledField> sample_gpu(const proc::FieldExpr& field, const proc::GridSpec& grid,
                                             std::string* error, const eval::CancellationToken* cancel,
                                             bool* cancelled) {
  State& s = state();
  Device* device = nullptr;
  std::size_t slab = 0;
  {
    std::lock_guard lock(s.mutex);
    device = device_locked(s);
    slab = s.slab_samples;
  }
  if (device == nullptr) {
    if (error != nullptr) *error = s.status.why;
    return std::nullopt;
  }
  FieldProgram program = translate(field);
  {
    std::lock_guard lock(s.mutex);
    if (s.failed_sources.contains(program.source)) {
      if (error != nullptr) *error = "the field's shader doesn't compile";
      return std::nullopt;
    }
  }
  program.d[0] = grid.origin.x;
  program.d[1] = grid.origin.y;
  program.d[2] = grid.origin.z;
  program.d[3] = grid.cell;

  proc::SampledField out;
  out.grid = grid;
  out.values.assign(grid.size(), 0.0f);
  if (cancelled != nullptr) *cancelled = false;
  std::string log;
  bool compiled = false;
  const std::size_t plane = static_cast<std::size_t>(grid.nx) * grid.ny;
  if (plane == 0 || grid.nz == 0) return out;

  device->run([&](const gl::Functions& f) {
    const gl::GLuint id = device->program(f, program.source, log);
    if (id == 0) return;
    compiled = true;
    Buffer d(f, program.d.size() * sizeof(double), program.d.data());
    Buffer u(f, program.u.size() * sizeof(std::uint32_t), program.u.data());
    // Slabs of whole z slices, at most what one dispatch can address.
    const std::size_t max_slices = std::max<std::size_t>(1, (65535ull * kLocalSize) / plane);
    std::size_t slices = std::clamp<std::size_t>(slab / plane, 1, std::min<std::size_t>(max_slices, grid.nz));
    Buffer o(f, std::min<std::size_t>(max_slices, grid.nz) * plane * sizeof(float));
    d.bind(0);
    u.bind(1);
    o.bind(2);
    f.UseProgram(id);
    f.Uniform1ui(f.GetUniformLocation(id, "nx"), grid.nx);
    f.Uniform1ui(f.GetUniformLocation(id, "ny"), grid.ny);
    const gl::GLint k0 = f.GetUniformLocation(id, "k0");
    const gl::GLint count = f.GetUniformLocation(id, "count");
    for (std::size_t k = 0; k < grid.nz;) {
      if (cancel != nullptr && cancel->cancelled()) {
        if (cancelled != nullptr) *cancelled = true;
        break;
      }
      const std::size_t n = std::min(slices, grid.nz - k);
      const std::size_t samples = n * plane;
      const auto t0 = std::chrono::steady_clock::now();
      f.Uniform1ui(k0, static_cast<gl::GLuint>(k));
      f.Uniform1ui(count, static_cast<gl::GLuint>(samples));
      f.DispatchCompute(static_cast<gl::GLuint>((samples + kLocalSize - 1) / kLocalSize), 1, 1);
      f.MemoryBarrier(gl::kBufferUpdateBarrierBit | gl::kShaderStorageBarrierBit);
      f.Finish();
      o.read(out.values.data() + k * plane, samples * sizeof(float));
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      k += n;
      // Aim the next slab at the target time.
      if (ms > 0.0) {
        const double scale = std::clamp(kTargetMs / ms, 0.25, 4.0);
        slices = std::clamp<std::size_t>(static_cast<std::size_t>(static_cast<double>(n) * scale), 1,
                                         std::min<std::size_t>(max_slices, grid.nz));
      }
    }
    std::lock_guard lock(s.mutex);
    s.slab_samples = slices * plane;
  });
  if (!compiled) {
    std::lock_guard lock(s.mutex);
    s.failed_sources.insert(program.source);
    if (error != nullptr) *error = log.empty() ? std::string("no device") : "the shader doesn't compile: " + log;
    return std::nullopt;
  }
  return out;
}

namespace {

// A closed box mesh, wound outward, and an open square patch.
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

// An icosahedron's sphere subdivided once: curved, with many triangles,
// so the BVH has depth.
mesh::Mesh ball_mesh(double radius, int subdivisions) {
  mesh::Mesh m;
  const double t = (1.0 + 2.2360679774997896964) / 2.0;
  std::vector<core::dvec3> p{{-1, t, 0}, {1, t, 0},  {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                             {0, -1, -t}, {0, 1, -t}, {t, 0, -1},  {t, 0, 1},  {-t, 0, -1}, {-t, 0, 1}};
  std::vector<std::array<std::uint32_t, 3>> f{{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                                              {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                              {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                                              {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
  for (int s = 0; s < subdivisions; ++s) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid;
    const auto middle = [&](std::uint32_t a, std::uint32_t b) {
      const auto key = std::pair{std::min(a, b), std::max(a, b)};
      if (const auto it = mid.find(key); it != mid.end()) return it->second;
      p.push_back((p[a] + p[b]) * 0.5);
      return mid[key] = static_cast<std::uint32_t>(p.size() - 1);
    };
    std::vector<std::array<std::uint32_t, 3>> next;
    for (const auto& tri : f) {
      const auto ab = middle(tri[0], tri[1]), bc = middle(tri[1], tri[2]), ca = middle(tri[2], tri[0]);
      next.push_back({tri[0], ab, ca});
      next.push_back({tri[1], bc, ab});
      next.push_back({tri[2], ca, bc});
      next.push_back({ab, bc, ca});
    }
    f = std::move(next);
  }
  for (const auto& q : p) m.add_vertex(core::to_fp32(normalize(q) * radius));
  const core::Id v = core::Id::random();
  for (const auto& tri : f) m.add_triangle(tri[0], tri[1], tri[2], v);
  return m;
}

}  // namespace

std::string self_check() {
  using namespace proc;
  const auto box = std::make_shared<const TriangleDistance>(box_mesh({-8, -6, -5}, {8, 6, 5}), true);
  const auto ball = std::make_shared<const TriangleDistance>(ball_mesh(7.0, 2), true);
  mesh::Mesh patch_mesh;
  patch_mesh.add_vertex({-4, -4, 0});
  patch_mesh.add_vertex({4, -4, 0});
  patch_mesh.add_vertex({4, 4, 0});
  patch_mesh.add_vertex({-4, 4, 0});
  patch_mesh.add_triangle(0, 1, 2, core::Id::random());
  patch_mesh.add_triangle(0, 2, 3, core::Id::random());
  const auto patch = std::make_shared<const TriangleDistance>(patch_mesh, false);

  FrameSet frames;
  for (int i = 0; i < 5; ++i) {
    core::ftransform t;
    const float a = 0.4f * static_cast<float>(i);
    t.linear.cols[0] = {std::cos(a), std::sin(a), 0};
    t.linear.cols[1] = {-std::sin(a), std::cos(a), 0};
    t.linear.cols[2] = {0, 0, 1};
    t.translation = {-6.0f + 3.0f * static_cast<float>(i), 1.0f, 0.5f};
    frames.frames.push_back(t);
  }

  const Field sphere = sphere_field({1, 2, 3}, 5.5);
  const Field boxf = box_field({0.5, -0.25, 0}, {7, 5, 3});
  const std::vector<std::pair<const char*, Field>> battery{
      {"constant", constant_field(1.25)},
      {"sphere", sphere},
      {"box", boxf},
      {"cylinder", cylinder_field({0, 0, 0}, {1, 2, 3}, 3.0, 9.0)},
      {"plane", plane_field({0, 1, 0}, {0.3, 1, 0.2})},
      {"half space", half_space_field({0, 0, 1}, {1, 1, 1})},
      {"booleans", difference_field(union_field(sphere, boxf), intersection_field(sphere, boxf))},
      {"smooth", smooth_difference_field(smooth_union_field(sphere, boxf, 2.0),
                                         smooth_intersection_field(sphere, boxf, 1.5), 0.75)},
      {"maths", clamp_field(add_field(scale_field(abs_field(negate_field(offset_field(shell_field(sphere, 1.0), 0.3))), 0.7),
                                      multiply_field(sphere, boxf)), -3.0, 4.0)},
      {"cubic lattice", lattice_field(LatticeCell::Cubic, 4.0, {0.1, 0.2, 0.3}, 0.8)},
      {"bcc lattice", lattice_field(LatticeCell::Bcc, 3.0, {0, 0, 0}, 0.6)},
      {"graded octet", lattice_field(LatticeCell::Octet, 5.0, {0, 0, 0}, 0, scale_field(sphere, 0.1))},
      {"gyroid", tpms_field(TpmsSurface::Gyroid, 6.0, {0.5, 0, 0}, 0.8)},
      {"schwarz p", tpms_field(TpmsSurface::SchwarzP, 5.0, {0, 0, 0}, 0.5)},
      {"graded diamond", tpms_field(TpmsSurface::Diamond, 7.0, {0, 1, 0}, 0, offset_field(boxf, -1.0))},
      {"box mesh", mesh_signed_field(box)},
      {"ball mesh", mesh_signed_field(ball)},
      {"patch", mesh_unsigned_field(patch)},
      {"infill", intersection_field(shell_field(mesh_signed_field(ball), 1.0),
                                    tpms_field(TpmsSurface::Gyroid, 4.0, {0, 0, 0}, 0.6))},
      {"instances", instances_field(sphere_field({0, 0, 0}, 1.5), frames, 1.5)},
      {"instances unbounded", instances_field(box_field({0, 0, 0}, {1, 2, 1}), frames, 0.0)},
  };
  const auto grid = proc::grid_covering(core::dbox3::from_min_max({-12, -10, -9}, {12, 10, 9}), 0.6);
  if (!grid) return "the check grid";
  for (const auto& [name, field] : battery) {
    std::string error;
    const auto gpu = sample_gpu(*field, *grid, &error);
    if (!gpu) return fmt::format("{}: {}", name, error);
    const proc::SampledField cpu = proc::sample_cpu(*field, *grid);
    std::size_t differ = 0;
    std::size_t first = 0;
    for (std::size_t i = 0; i < cpu.values.size(); ++i) {
      if (std::bit_cast<std::uint32_t>(cpu.values[i]) != std::bit_cast<std::uint32_t>(gpu->values[i])) {
        if (differ++ == 0) first = i;
      }
    }
    if (differ > 0) {
      return fmt::format("{}: {} of {} samples differ (first at {}: CPU {} GPU {})", name, differ,
                         cpu.values.size(), first, cpu.values[first], gpu->values[first]);
    }
  }
  return {};
}

Status install(const InstallOptions& options) {
  State& s = state();
  bool run_check = false;
  {
    std::lock_guard lock(s.mutex);
    s.options = options;
    if (s.status.installed) return s.status;
    if (device_locked(s) == nullptr) return s.status;
    run_check = options.check;
  }
  if (run_check) {
    const std::string mismatch = self_check();
    if (!mismatch.empty()) {
      std::lock_guard lock(s.mutex);
      s.status.why = "the GPU's samples don't match the CPU's (" + mismatch + ")";
      return s.status;
    }
  }
  proc::set_sampling_backend(std::make_shared<GpuBackend>());
  std::lock_guard lock(s.mutex);
  s.status.installed = true;
  s.status.why.clear();
  return s.status;
}

void uninstall() {
  proc::set_sampling_backend(nullptr);
  std::lock_guard lock(state().mutex);
  state().status.installed = false;
}

Status status() {
  std::lock_guard lock(state().mutex);
  return state().status;
}

}  // namespace vs::gpu

#include "vs/gpu/gpu.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include <fmt/format.h>

#include "device.hpp"
#include "glsl.hpp"
#include "state.hpp"
#include "vs/core/transform.hpp"
#include "vs/eval/thread_pool.hpp"
#include "vs/proc/distance.hpp"
#include "vs/proc/mesher.hpp"

namespace vs::gpu {

namespace {

struct State {
  std::mutex mutex;
  std::shared_ptr<Device> device;
  bool tried = false;
  bool shut_down = false;
  Status status;
  InstallOptions options;
  std::set<std::string> failed_sources;  // shaders that didn't compile
  std::size_t slab_samples = 1 << 16;    // adapted to keep dispatches short
  // Held for the whole of install(), so two callers never check and
  // calibrate at once; taken before `mutex`, never while holding it.
  std::mutex install_mutex;
};

// Never destroyed: the device thread and opengl32.dll must not be torn
// down by static destructors at exit (shutdown() does that, in order).
State& state() {
  static State* s = new State;
  return *s;
}

std::shared_ptr<Device> device_locked(State& s) {
  if (!s.tried && !s.shut_down) {
    s.tried = true;
    s.device = std::make_shared<Device>();
    if (s.device->ok()) {
      s.status.device = s.device->description();
    } else {
      s.status.why = s.device->why();
    }
  }
  return s.device && s.device->ok() ? s.device : nullptr;
}

// Whether a grid of `samples` points goes to the GPU for a stage.
bool wanted(bool Status::*stage, std::size_t samples) {
  std::lock_guard lock(state().mutex);
  return state().status.*stage && samples >= state().options.min_samples;
}

class GpuBackend final : public proc::SamplingBackend {
 public:
  bool sample(const proc::FieldExpr& field, const proc::GridSpec& grid, std::vector<float>& values,
              const eval::CancellationToken* cancel, bool* cancelled) override {
    if (!wanted(&Status::sampling, grid.size())) return false;
    auto r = sample_gpu(field, grid, nullptr, cancel, cancelled);
    if (!r) return false;
    values = std::move(r->values);
    return true;
  }
};

class GpuMeshingBackend final : public proc::MeshingBackend {
 public:
  bool clip(const proc::SampledField& samples, const proc::TriangleDistance& boundary, double inset,
            std::vector<float>& values) override {
    if (!wanted(&Status::clip, samples.grid.size())) return false;
    auto r = clip_gpu(samples, boundary, inset);
    if (!r) return false;
    values = std::move(*r);
    return true;
  }
  bool unmerged_net(const proc::SampledField& samples, proc::UnmergedNet& net) override {
    if (!wanted(&Status::meshing, samples.grid.size())) return false;
    auto r = unmerged_net_gpu(samples);
    if (!r) return false;
    net = std::move(*r);
    return true;
  }
};

// Target time per dispatch; Windows resets the GPU after 2 s.
constexpr double kTargetMs = 100.0;

}  // namespace

std::shared_ptr<Device> shared_device(std::string* why) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  auto d = device_locked(s);
  if (d == nullptr && why != nullptr) *why = s.status.why;
  return d;
}

std::optional<proc::SampledField> sample_gpu(const proc::FieldExpr& field, const proc::GridSpec& grid,
                                             std::string* error, const eval::CancellationToken* cancel,
                                             bool* cancelled) {
  State& s = state();
  const auto fail = [&](std::string why) -> std::optional<proc::SampledField> {
    if (error != nullptr) *error = std::move(why);
    return std::nullopt;
  };
  std::string why;
  const std::shared_ptr<Device> device = shared_device(&why);
  if (device == nullptr) return fail(why);
  std::size_t slab = 0;
  {
    std::lock_guard lock(s.mutex);
    slab = s.slab_samples;
  }
  FieldProgram program = translate(field);
  {
    std::lock_guard lock(s.mutex);
    if (s.failed_sources.contains(program.source)) return fail("the field's shader doesn't compile");
  }
  program.d[0] = grid.origin.x;
  program.d[1] = grid.origin.y;
  program.d[2] = grid.origin.z;
  program.d[3] = grid.cell;

  proc::SampledField out;
  out.grid = grid;
  if (cancelled != nullptr) *cancelled = false;
  const std::size_t plane = static_cast<std::size_t>(grid.nx) * grid.ny;
  if (plane == 0 || grid.nz == 0) return out;
  // Every buffer within what a shader may address, and a slab's sample
  // indices within 32 bits.
  const std::size_t max_block = device->max_block_bytes();
  const std::size_t d_bytes = program.d.size() * sizeof(double);
  const std::size_t u_bytes = program.u.size() * sizeof(std::uint32_t);
  if (d_bytes > max_block || u_bytes > max_block) {
    return fail(fmt::format("the field's data ({} MB) is larger than a GPU storage block ({} MB)",
                            std::max(d_bytes, u_bytes) >> 20, max_block >> 20));
  }
  const std::size_t max_slices =
      std::min<std::size_t>(max_block / (plane * sizeof(float)), ((std::size_t{1} << 32) - 1) / plane);
  if (max_slices == 0) {
    return fail(fmt::format("a z slice of {} x {} samples is larger than a GPU storage block", grid.nx, grid.ny));
  }
  out.values.assign(grid.size(), 0.0f);

  std::string log;
  std::string gl_error;
  bool compiled = false;
  device->run([&](const gl::Functions& f) {
    while (f.GetError() != gl::kNoError) {
    }
    const gl::GLuint id = device->program(f, program.source, log);
    if (id == 0) return;
    compiled = true;
    Buffer d(f, d_bytes, program.d.data());
    Buffer u(f, u_bytes, program.u.data());
    // Slabs of whole z slices.
    std::size_t slices = std::clamp<std::size_t>(slab / plane, 1, std::min<std::size_t>(max_slices, grid.nz));
    Buffer o(f, std::min<std::size_t>(max_slices, grid.nz) * plane * sizeof(float));
    gl_error = gl_errors(f);
    if (!gl_error.empty()) return;
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
      // Two dimensions, so a wide slice never needs more than 65,535
      // workgroups along one.
      const Dispatch groups = dispatch_2d((samples + kLocalSize - 1) / kLocalSize);
      f.DispatchCompute(groups.x, groups.y, 1);
      f.MemoryBarrier(gl::kBufferUpdateBarrierBit | gl::kShaderStorageBarrierBit);
      f.Finish();
      o.read(out.values.data() + k * plane, samples * sizeof(float));
      // Never silent zeros: a dispatch or read GL refused fails the call.
      gl_error = gl_errors(f);
      if (!gl_error.empty()) return;
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
    if (log.empty()) return fail("the GPU device stopped");
    std::lock_guard lock(s.mutex);
    s.failed_sources.insert(program.source);
    return fail("the shader doesn't compile: " + log);
  }
  if (!gl_error.empty()) return fail("OpenGL error " + gl_error);
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

  // Seeds for the Voronoi walls: a lattice nudged off its grid (so no two
  // points tie), and a flat layer, as an offset surface makes.
  FrameSet cloud;
  FrameSet layer;
  for (int i = 0; i < 216; ++i) {
    const int x = i % 6, y = (i / 6) % 6, z = i / 36;
    core::ftransform t;
    t.translation = {-10.0f + 4.0f * static_cast<float>(x) + 0.37f * static_cast<float>((i * 7) % 5),
                     -8.0f + 3.2f * static_cast<float>(y) + 0.29f * static_cast<float>((i * 3) % 7),
                     -7.5f + 3.0f * static_cast<float>(z) + 0.41f * static_cast<float>((i * 5) % 3)};
    cloud.frames.push_back(t);
    if (z == 0) {
      t.translation.z = 1.5f;
      layer.frames.push_back(t);
    }
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
      {"voronoi", voronoi_field(frames, 0.7)},
      {"voronoi cloud", voronoi_field(cloud, 0.5)},
      {"voronoi layer in a ball", intersection_field(mesh_signed_field(ball), voronoi_field(layer, 0.9))},
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

namespace {

// Grids for the meshing check: noise (every tangled cube), noise with
// exact zeros, smooth blobs, all of odd sizes.
proc::SampledField check_grid(std::uint64_t seed, std::uint32_t nx, std::uint32_t ny, std::uint32_t nz, int kind) {
  proc::SampledField s;
  s.grid.origin = {-3.25, 1.5, -0.75};
  s.grid.cell = 0.6;
  s.grid.nx = nx;
  s.grid.ny = ny;
  s.grid.nz = nz;
  s.values.resize(s.grid.size());
  std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
  const auto next = [&] {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return static_cast<float>(static_cast<double>(x >> 11) / 9007199254740992.0 * 2.0 - 1.0);
  };
  for (std::uint32_t k = 0; k < nz; ++k) {
    for (std::uint32_t j = 0; j < ny; ++j) {
      for (std::uint32_t i = 0; i < nx; ++i) {
        float v = next();
        if (kind == 1 && (i + j + k) % 3 == 0) v = 0.0f;
        if (kind == 2) {
          const double fx = static_cast<double>(i) * 0.37;
          const double fy = static_cast<double>(j) * 0.29;
          const double fz = static_cast<double>(k) * 0.31;
          v = static_cast<float>(std::sin(fx) * std::cos(fy) + std::sin(fy) * std::cos(fz) +
                                 std::sin(fz) * std::cos(fx) + 0.05 * v);
        }
        s.values[s.grid.index(i, j, k)] = v;
      }
    }
  }
  return s;
}

// The first difference between two unmerged nets, in words.
std::string difference(const proc::UnmergedNet& gpu, const proc::UnmergedNet& cpu) {
  const auto first = [](const auto& a, const auto& b, const char* what) -> std::string {
    if (a.size() != b.size()) return fmt::format("{} {} on the GPU, {} on the CPU", a.size(), what, b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (!(a[i] == b[i])) return fmt::format("{} {} differ", what, i);
    }
    return {};
  };
  for (const std::string& s : {first(gpu.sums, cpu.sums, "vertex sums"), first(gpu.counts, cpu.counts, "counts"),
                               first(gpu.group_starts, cpu.group_starts, "group starts"),
                               first(gpu.cubes, cpu.cubes, "cubes")}) {
    if (!s.empty()) return s;
  }
  if (gpu.triangles.size() != cpu.triangles.size()) {
    return fmt::format("{} triangles on the GPU, {} on the CPU", gpu.triangles.size(), cpu.triangles.size());
  }
  for (std::size_t t = 0; t < gpu.triangles.size(); ++t) {
    const auto& g = gpu.triangles[t];
    const auto& c = cpu.triangles[t];
    if (g != c) {
      return fmt::format("triangle {} is {} {} {} on the GPU, {} {} {} on the CPU", t, g[0], g[1], g[2], c[0], c[1],
                         c[2]);
    }
  }
  return "nothing differs";
}

}  // namespace

std::string meshing_self_check() {
  using namespace proc;
  // The unmerged net, element for element.
  const std::uint32_t sizes[][3] = {{7, 5, 6}, {13, 9, 11}, {24, 31, 17}, {40, 33, 29}};
  int round = 0;
  for (const auto& n : sizes) {
    for (int kind = 0; kind < 3; ++kind, ++round) {
      const SampledField s = check_grid(static_cast<std::uint64_t>(round) + 17, n[0], n[1], n[2], kind);
      std::string error;
      const auto gpu = unmerged_net_gpu(s, &error);
      if (!gpu) return fmt::format("unmerged net {}: {}", round, error);
      const UnmergedNet cpu = unmerged_net_cpu(s);
      if (!(*gpu == cpu)) return fmt::format("unmerged net {} ({} x {} x {}): {}", round, n[0], n[1], n[2], difference(*gpu, cpu));
    }
  }
  // A gyroid infill clipped to a ball and a box.
  const auto ball = std::make_shared<const TriangleDistance>(ball_mesh(9.0, 3), true);
  const auto box = std::make_shared<const TriangleDistance>(box_mesh({-8, -6, -5}, {8, 6, 5}), true);
  const Field infill = tpms_field(TpmsSurface::Gyroid, 4.0, {0.3, 0, 0}, 0.7);
  const auto grid = grid_covering(core::dbox3::from_min_max({-10, -10, -10}, {10, 10, 10}), 0.35);
  if (!grid) return "the meshing check grid";
  const SampledField samples = sample_cpu(*infill, *grid);
  for (const auto& [name, boundary] : {std::pair{"ball", ball}, std::pair{"box", box}}) {
    std::string error;
    const auto clipped = clip_gpu(samples, *boundary, kContentsInset, &error);
    if (!clipped) return fmt::format("clip to the {}: {}", name, error);
    const SampledField cpu = clip_to_boundary_cpu(samples, *boundary, kContentsInset);
    for (std::size_t i = 0; i < cpu.values.size(); ++i) {
      if (std::bit_cast<std::uint32_t>(cpu.values[i]) != std::bit_cast<std::uint32_t>((*clipped)[i])) {
        return fmt::format("clip to the {}: sample {} is {} on the CPU, {} on the GPU", name, i, cpu.values[i],
                           (*clipped)[i]);
      }
    }
    const auto gpu_net = unmerged_net_gpu(cpu, &error);
    if (!gpu_net) return fmt::format("gyroid in the {}: {}", name, error);
    const UnmergedNet cpu_net = unmerged_net_cpu(cpu);
    if (!(*gpu_net == cpu_net)) return fmt::format("gyroid in the {}: {}", name, difference(*gpu_net, cpu_net));
  }
  return {};
}

namespace {

double ms_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// The fastest of a few runs of `f`, in milliseconds; infinite when a run
// failed (`f` returned false), so a failing stage never wins.
template <typename F>
double best_of(int runs, const F& f) {
  double best = std::numeric_limits<double>::infinity();
  for (int r = 0; r < runs; ++r) {
    const auto t = std::chrono::steady_clock::now();
    if (!f()) return std::numeric_limits<double>::infinity();
    best = std::min(best, ms_since(t));
  }
  return best;
}

struct Calibration {
  bool sampling = false;
  bool clip = false;
  bool meshing = false;
  std::string text;
};

// Each stage on a gyroid infill in a ball, half a million samples: on the GPU
// (the better of two runs, so shader compiles don't count) and on every
// CPU thread. The GPU takes a stage only when it ran and was faster.
Calibration calibrate(bool meshing_ok) {
  using namespace proc;
  const auto ball = std::make_shared<const TriangleDistance>(ball_mesh(24.0, 3), true);
  const Field field = intersection_field(shell_field(mesh_signed_field(ball), 1.2),
                                         tpms_field(TpmsSurface::Gyroid, 6.0, {0, 0, 0}, 0.8));
  const auto grid = grid_covering(core::dbox3::from_min_max({-25, -25, -25}, {25, 25, 25}), 0.625);
  Calibration c;
  if (!grid) return c;
  eval::ThreadPool pool(0);
  const auto row = [&](const char* name, double gpu, double cpu) {
    const std::string g = std::isfinite(gpu) ? fmt::format("GPU {:.0f} ms", gpu) : std::string("GPU failed");
    c.text += fmt::format("{}{} {}, CPU {:.0f} ms", c.text.empty() ? "" : "; ", name, g, cpu);
    return gpu_wins(gpu, cpu);
  };
  SampledField samples;
  const double s_gpu = best_of(2, [&] { return sample_gpu(*field, *grid).has_value(); });
  const double s_cpu = best_of(1, [&] {
    samples = sample_cpu(*field, *grid, &pool);
    return true;
  });
  c.sampling = row("sampling", s_gpu, s_cpu);
  if (!meshing_ok) return c;
  SampledField clipped;
  const double c_gpu = best_of(2, [&] { return clip_gpu(samples, *ball, kContentsInset).has_value(); });
  const double c_cpu = best_of(1, [&] {
    clipped = clip_to_boundary_cpu(samples, *ball, kContentsInset, &pool);
    return true;
  });
  c.clip = row("clip", c_gpu, c_cpu);
  const double n_gpu = best_of(2, [&] { return unmerged_net_gpu(clipped).has_value(); });
  const double n_cpu = best_of(1, [&] {
    unmerged_net_cpu(clipped, &pool);
    return true;
  });
  c.meshing = row("unmerged net", n_gpu, n_cpu);
  return c;
}

}  // namespace

bool gpu_wins(double gpu_ms, double cpu_ms) { return std::isfinite(gpu_ms) && gpu_ms < cpu_ms; }

Status install(const InstallOptions& options) {
  State& s = state();
  // One install at a time: a second caller waits, then sees the first's
  // status.
  std::lock_guard installing(s.install_mutex);
  bool run_check = false;
  {
    std::lock_guard lock(s.mutex);
    s.options = options;
    if (s.status.installed) {
      if (options.force) {
        s.status.sampling = true;
        s.status.clip = s.status.meshing = s.status.why.empty();
      }
      return s.status;
    }
    if (device_locked(s) == nullptr) return s.status;
    run_check = options.check;
  }
  const auto t0 = std::chrono::steady_clock::now();
  if (run_check) {
    const std::string mismatch = self_check();
    if (!mismatch.empty()) {
      std::lock_guard lock(s.mutex);
      s.status.why = "the GPU's samples don't match the CPU's (" + mismatch + ")";
      return s.status;
    }
  }
  std::string meshing_why;
  if (run_check) {
    const std::string mismatch = meshing_self_check();
    if (!mismatch.empty()) meshing_why = "the GPU's meshes don't match the CPU's (" + mismatch + ")";
  }
  Calibration c;
  if (options.force) {
    c.sampling = true;
    c.clip = c.meshing = meshing_why.empty();
    c.text = "forced";
  } else {
    c = calibrate(meshing_why.empty());
  }
  {
    // Shut down meanwhile: install nothing.
    std::lock_guard lock(s.mutex);
    if (s.shut_down) return s.status;
  }
  proc::set_sampling_backend(std::make_shared<GpuBackend>());
  proc::set_meshing_backend(std::make_shared<GpuMeshingBackend>());
  std::lock_guard lock(s.mutex);
  s.status.installed = true;
  s.status.why = meshing_why;
  s.status.sampling = c.sampling;
  s.status.clip = c.clip;
  s.status.meshing = c.meshing;
  s.status.calibration = c.text;
  s.status.install_ms = ms_since(t0);
  return s.status;
}

void uninstall() {
  proc::set_sampling_backend(nullptr);
  proc::set_meshing_backend(nullptr);
  std::lock_guard lock(state().mutex);
  state().status.installed = false;
  state().status.sampling = false;
  state().status.clip = false;
  state().status.meshing = false;
}

void shutdown() {
  State& s = state();
  uninstall();
  std::shared_ptr<Device> device;
  {
    std::lock_guard lock(s.mutex);
    s.shut_down = true;
    s.tried = true;
    if (s.device) s.status.why = "the GPU was shut down";
    device = std::move(s.device);
  }
  // The last holder closes the device: here, or a call still running on
  // another thread, when it returns.
  device.reset();
}

Status status() {
  std::lock_guard lock(state().mutex);
  return state().status;
}

}  // namespace vs::gpu

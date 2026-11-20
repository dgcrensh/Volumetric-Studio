#pragma once

// GPU field sampling (L1; plan docs/plans/L1-gpu-field-evaluation.md,
// decision 0018) and meshing (L1b; plan
// docs/plans/L1b-parallel-and-gpu-meshing.md, decision 0019).
//
// install() opens an OpenGL 4.3 compute context on a thread of its own,
// checks that the GPU samples a battery of fields (every field operation,
// lattices, TPMS, boundary distances, instances) to exactly the CPU's
// values, and only then installs itself as vs_proc's sampling backend.
// It also checks the boundary clip and the unmerged surface net against
// the CPU's, element for element, and installs the meshing backend when
// they match. Then it times each stage (sampling, clip, unmerged net) on a
// calibration grid on the GPU and on every CPU thread, and uses the GPU
// only for the stages where it was faster: GPUs with slow FP64 (most
// consumer cards) can lose to a many-core CPU. Either way the samples,
// meshes, hashes and everything made from them are identical. A machine
// without a usable GPU, or whose GPU doesn't match the CPU, keeps
// everything on the CPU, and status() says why.
//
// Each field tree is translated into one compute shader (one function per
// node, numbers in a storage buffer), so a tree compiles once per shape and
// parameter edits reuse it. Grids go in slabs of z slices, each dispatch a
// fraction of a second (well inside Windows' GPU timeout), with
// cancellation checked between slabs.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "vs/eval/cancel.hpp"
#include "vs/proc/distance.hpp"
#include "vs/proc/field.hpp"
#include "vs/proc/mesher.hpp"

namespace vs::gpu {

struct Status {
  bool installed = false;  // the checks passed: the backends are installed
  std::string device;      // "NVIDIA L4/PCIe/SSE2, 4.6.0 NVIDIA 596.86", when there is one
  std::string why;         // why nothing runs on the GPU, or why meshing doesn't
  // L1b: the stages the GPU takes (each matched the CPU and, unless
  // forced, measured faster on the calibration grid).
  bool sampling = false;
  bool clip = false;
  bool meshing = false;  // the unmerged surface net
  // The calibration's timings, "sampling GPU 21 ms, CPU 40 ms; ...", and
  // how long the checks and calibration took altogether.
  std::string calibration;
  double install_ms = 0.0;
};

struct InstallOptions {
  // Smaller grids stay on the CPU, where they cost less than a dispatch.
  std::size_t min_samples = 32768;
  // Run the self-checks (tests may skip them to see raw mismatches).
  bool check = true;
  // Use the GPU for every stage that matches the CPU, faster or not
  // (benchmarks and tests; `vstudio-cli --gpu=force`). Skips calibration.
  bool force = false;
};

// Idempotent: a second call returns the first one's status (with new
// options applied; `force` turns on every checked stage).
Status install(const InstallOptions& options = {});
// Removes the backends; vs_proc uses the CPU again. The device stays open
// for the *_gpu() functions.
void uninstall();
Status status();

// Samples on the GPU whatever the size, or nullopt (with `error`) when
// there is no device or the shader doesn't compile. For tests, benchmarks
// and the self-check.
std::optional<proc::SampledField> sample_gpu(const proc::FieldExpr& field, const proc::GridSpec& grid,
                                             std::string* error = nullptr,
                                             const eval::CancellationToken* cancel = nullptr,
                                             bool* cancelled = nullptr);

// proc::clip_to_boundary_cpu()'s values, on the GPU (L1b).
std::optional<std::vector<float>> clip_gpu(const proc::SampledField& samples,
                                           const proc::TriangleDistance& boundary, double inset,
                                           std::string* error = nullptr);

// proc::unmerged_net_cpu()'s net, on the GPU (L1b); nullopt (with
// `error`) when there is no device, a shader doesn't compile, or the
// grid's net wouldn't fit in the GPU's buffers.
std::optional<proc::UnmergedNet> unmerged_net_gpu(const proc::SampledField& samples,
                                                  std::string* error = nullptr);

// The self-checks alone: the battery sampled on both sides; the clip and
// unmerged net of a battery of grids on both sides. Empty when everything
// matches, else what differed first.
std::string self_check();
std::string meshing_self_check();

}  // namespace vs::gpu

#pragma once

// GPU field sampling (L1; plan docs/plans/L1-gpu-field-evaluation.md,
// decision 0018).
//
// install() opens an OpenGL 4.3 compute context on a thread of its own,
// checks that the GPU samples a battery of fields (every field operation,
// lattices, TPMS, boundary distances, instances) to exactly the CPU's
// values, and only then installs itself as vs_proc's sampling backend.
// From then on proc::sample() sends grids of at least `min_samples` points
// to the GPU and the rest to the CPU; either way the samples, their hashes
// and everything made from them are identical. A machine without a usable
// GPU, or whose GPU doesn't match the CPU, keeps sampling on the CPU, and
// status() says why.
//
// Each field tree is translated into one compute shader (one function per
// node, numbers in a storage buffer), so a tree compiles once per shape and
// parameter edits reuse it. Grids go in slabs of z slices, each dispatch a
// fraction of a second (well inside Windows' GPU timeout), with
// cancellation checked between slabs.

#include <cstddef>
#include <optional>
#include <string>

#include "vs/eval/cancel.hpp"
#include "vs/proc/field.hpp"

namespace vs::gpu {

struct Status {
  bool installed = false;  // proc::sample() uses the GPU
  std::string device;      // "NVIDIA L4/PCIe/SSE2, 4.6.0 NVIDIA 596.86", when there is one
  std::string why;         // why it isn't installed
};

struct InstallOptions {
  // Smaller grids stay on the CPU, where they cost less than a dispatch.
  std::size_t min_samples = 32768;
  // Run the self-check (tests may skip it to see raw mismatches).
  bool check = true;
};

// Idempotent: a second call returns the first one's status (with new
// options applied).
Status install(const InstallOptions& options = {});
// Removes the backend; proc::sample() uses the CPU again. The device stays
// open for sample_gpu().
void uninstall();
Status status();

// Samples on the GPU whatever the size, or nullopt (with `error`) when
// there is no device or the shader doesn't compile. For tests, benchmarks
// and the self-check.
std::optional<proc::SampledField> sample_gpu(const proc::FieldExpr& field, const proc::GridSpec& grid,
                                             std::string* error = nullptr,
                                             const eval::CancellationToken* cancel = nullptr,
                                             bool* cancelled = nullptr);

// The self-check alone: the battery sampled on both sides. Empty when
// every value matches, else what differed first.
std::string self_check();

}  // namespace vs::gpu

# 0018: GPU field evaluation

Nov 20, 2026 · L1 · plan: `docs/plans/L1-gpu-field-evaluation.md`

## Questions

1. The plan asked for GPU sampling that "must match CPU hashes or document
   the quantisation". Can a GPU give the CPU's FP64 results bit for bit, or
   do hashes need quantising?
2. Which API, given that every library must build headless on Linux, the VM
   session can't install SDKs, and the field code is FP64?
3. How does `vs_proc`, below any GPU code in the module order, use it?
4. How are field trees, which a graph builds freely, run on the GPU without
   recompiling a shader for every parameter change?

## Decisions

**Exact, not quantised.** The spike (`vs_gpu_tests "[arithmetic]"`) ran
4 million random operand pairs from 2^-60 to 2^60 through GLSL FP64 on the
L4: `+ - * /`, `floor`, `abs`, comparisons and the rounding to FP32 give
the CPU's bits, as long as every double is `precise` (no contraction into
fused multiply-adds; the CPU build has FMA off too). Only `sqrt` differs,
about a third of results by an ulp, so the shaders use a correctly rounded
square root: the hardware's value, then each neighbour tested against the
exact residual `fma(-c, c, x)` (exact for c within an ulp of the root),
moving up when `x - c*c > c*ulp_up` and down when `x - c*c <= -c*ulp_down`.
With it every sample, hash, mesh and cache key is what the CPU makes, so
nothing downstream changed and no golden value moved.

**OpenGL 4.3 compute, loaded at run time.** A hidden window and WGL give a
context on Windows; the ~30 entry points are loaded with
`wglGetProcAddress`, and the types and constants are declared in
`src/gl.hpp`, so no GL header or SDK is needed and Linux builds the library
unchanged (its context layer says "only implemented on Windows so far" and
the CPU samples). Vulkan would need an SDK and SPIR-V tooling; Direct3D's
HLSL has no FP64 square root; CUDA isn't installed and can't be without
admin rights. GLSL FP64 has everything the field code uses. The context
lives on a thread of its own (`Device`); callers queue jobs and wait.

**A backend hook in `vs_proc`, the GPU in a new library above it.**
`proc::sample()` offers each grid to an installed `SamplingBackend` first
and samples on the CPU whatever it declines (`sample_cpu()` is the CPU path
on its own). `vs_gpu`, last in the module order, implements it;
`gpu::install()` opens the device, runs the self-check and installs the
backend. Grids under 32,768 samples stay on the CPU, where a dispatch costs
more than it saves.

**One shader per tree shape.** `translate()` (`src/glsl.cpp`) writes a GLSL
function per distinct node, children first, repeating `FieldExpr::eval()`
operation for operation: `std::min`/`max`/`clamp` as the C++ library
writes them (not GLSL's), `dot` summed in the same order, `sin_det` /
`cos_det` with the same constants, the same lattice struts, TPMS, smooth
booleans and instance skipping. Every constant is written as its bit
pattern (`packDouble2x32`), never a decimal the driver parses. Each node's
numbers sit in a storage buffer reached through an offset table, so the
source depends only on the tree's shape: editing or dragging a parameter
reuses the compiled program. Compiled programs are kept per source text.

**Boundary distances walk the same BVH.** `TriangleDistance` exposes its
nodes, triangles, pseudonormals and leaf order read-only; the shader walks
them with an explicit stack that visits nodes in the recursion's order
(nearer child first, the far child only while its box can still beat the
best so far), so ties pick the same triangle and the sign comes from the
same pseudonormal.

**Slabs, a finish per slab, and a self-check.** Grids go out as slabs of
whole z slices, sized to about 100 ms each (well inside Windows' 2-second
GPU timeout) and capped at 65,535 work groups; cancellation is checked
between slabs. Each slab ends with `glFinish()` before reading back:
without it, large first dispatches on the L4 sometimes read back zeros
past the first slices, which the self-check caught. `install()` samples a
battery of 21 fields (every operation, three lattices and TPMS, graded
ones, box and sphere meshes, an open patch, an infill, instances with and
without a reach) on both sides and installs only when every value matches;
otherwise `status().why` says what differed and the CPU keeps sampling.

**Where it's on.** The app installs the backend at start on a background
thread (until the check passes, the CPU samples; `--cpu` skips it) and the
About box says which device samples fields or why the CPU does.
`vstudio-cli --gpu <command>` does the same for any command.

## Results (NVIDIA L4, driver 596.86, 4 vCPUs)

- Every field operation, the three M12 templates and the two procedural
  sample projects give identical samples and output hashes on the GPU and
  the CPU (`vs_gpu_tests`).
- A 40 x 30 x 20 mm gyroid at 0.25 mm (1.58 million samples): 22 ms on the
  GPU against 353 ms on one CPU thread.
- `bench-gyroid --size 100 --cell 0.5` (8.1 million samples, 9.6 million
  triangles), CPU against `--gpu`: sampling 3.93 s on 4 threads against
  1.39 s, the same 415.9 g part. The engine total only drops from 43.7 s to
  39.6 s, because surface nets (26 s) and the boundary clip (1.4 s) still
  run on the CPU. (The VM is slower than the desktop that set the M12
  baseline of 23.9 s.)

## What's next

Sampling is no longer the slow part of a large infill: meshing is. GPU
surface nets (or at least the per-cell sign tests), and clipping against
the boundary on the GPU, are the follow-ups worth doing; both must keep the
CPU's exact output, as sampling does. An EGL context would bring the GPU to
Linux machines with one.

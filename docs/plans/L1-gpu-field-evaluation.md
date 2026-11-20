# L1 — GPU field evaluation (plan)

Nov 20, 2026 · follows M17 · decision record: `docs/decisions/0018-gpu-field-evaluation.md`

## Goal

Sample procedural fields on the GPU, so lattice and TPMS infills over large
volumes regenerate several times faster, with **exactly the CPU's samples**:
same FP32 values, same hashes, same meshes, same caches. If exactness can't
be had on a machine, the CPU keeps doing the work there; nothing is ever
quantised.

## What the spike established (Nov 20, 2026, NVIDIA L4, driver 596.86)

GLSL 4.30 compute shaders in FP64 with `precise` (no contraction into fused
multiply-adds) give the CPU's bits for `+ - * /`, `floor` and the rounding
to FP32, over 4 million random operands from 2^-60 to 2^60. `sqrt` doesn't
(about 30% of results are an ulp off), so the shaders use their own
correctly rounded square root: the hardware's result, then a check of the
neighbouring doubles against the exact residual `fma(-c, c, x)`. With it,
every operation the field code uses is bit-identical.
`libs/vs_gpu/tests/test_arithmetic.cpp` keeps that measurement.

## Design

- **A new library, `vs_gpu`**, after `vs_proc` in the module order. It owns
  a headless OpenGL 4.3 context (a hidden window and WGL on Windows; other
  platforms report "not implemented" and use the CPU) on a thread of its
  own. GL entry points are loaded at run time, so no GL headers or SDK are
  needed and Linux CI builds it unchanged.
- **A sampling backend hook in `vs_proc`.** `sample()` offers each grid to
  an installed `SamplingBackend` first; the backend takes it or declines
  (unsupported field, grid too small to be worth it, no device), and the
  CPU does what it declines. `vs_gpu::install()` installs the GPU backend;
  nothing below vs_gpu changes behaviour.
- **Field trees become GLSL.** Each tree is translated into one shader:
  a function per node, with every number read from a storage buffer (so
  changing a parameter, or dragging, reuses the compiled program; only a
  change of the tree's shape compiles a new one). The shader repeats the
  CPU code operation by operation: the same order of additions in `dot`,
  `std::min`/`std::max`/`std::clamp` as written (not GLSL's), `sin_det` /
  `cos_det` with the same constants, the lattice struts, TPMS, smooth
  booleans and instances with the same skip test.
- **Mesh distances on the GPU.** A boundary's signed distance (the
  pseudonormal BVH, `TriangleDistance`) is uploaded as flat arrays and
  walked with an explicit stack in the CPU's order (nearer child first, the
  far child only while it can still win), so ties pick the same triangle.
  `TriangleDistance` gains a read-only view of its arrays for this.
- **Safety net.** On install the backend samples a battery of fields (every
  operation, a lattice, a TPMS, a boundary distance, instances) on both
  sides and enables itself only if every value matches. Tests repeat that
  per operation and through the engine (the M12 templates' contents hashes
  with and without the GPU).
- **Chunks.** Grids go to the GPU in slabs of z slices sized to stay well
  under Windows' 2-second GPU timeout; cancellation is checked between
  slabs.
- **Where it's used.** The app installs the backend at start (the About box
  says which device samples fields, or why the CPU does); `vstudio-cli`
  gets `--gpu` on `rebuild`, `inspect` and `bench-gyroid`, so the M12 CPU
  baseline (100 mm gyroid at 0.5 mm: 23.9 s on 4 threads) gets a GPU
  number beside it.

## Acceptance

- Every field operation, and each M12 template, samples to the same hash
  on the GPU as on the CPU (tests, on the VM's L4).
- The engine's contents hashes for the procedural sample projects are the
  same with and without the GPU backend.
- `bench-gyroid --gpu` is faster than the CPU baseline on the VM, and the
  result is recorded in the decision.
- Without a usable GPU everything works as before, on the CPU, and says why.

## Out of scope

- Meshing on the GPU (surface nets stays on the CPU).
- Non-Windows GPU contexts (EGL on Linux is a small follow-up: only
  `gl_context.cpp` changes).
- FP32 fields or any quantised mode.

# 0025: H3, determinism and cache keys (`kEngineVersion = 2`)

Nov 21, 2026 · plan `docs/plans/H-hardening.md` (PR 28), phase H3

## Context

H3 removes the cross-platform nondeterminism left in the boundary
distance, makes cache keys complete and bumps the engine version once. As
in H1 and H2, every item starts with a test that fails on `main`
(`612686e`); an item that can't be reproduced is dropped and says so here.
Everything is CPU-side and FP64; the GPU's shader source is unchanged.

## Decisions

**BVH leaves are sorted.** `TriangleDistance::build()` splits with
`std::nth_element` and a total order (ties by index), so which triangles
land in each leaf is the same everywhere, but their order inside it was
whatever the standard library left: 813 of 2,047 leaves came out unsorted
on a random mesh with libstdc++. The search keeps the first of two equally
near triangles, so the nearest triangle (and through it the pseudonormal
and the sign) could differ between GCC, Clang and MSVC. Each leaf is now
sorted by triangle index. vs_gpu walks the order the CPU uploads, so it
follows with no shader change. `kBvhLeafSize` (4) and `kMaxBvhDepth` (49,
the same `depth > 48` as before) are public in `distance.hpp`.

**No libm in pseudonormals.** `corner_angle()` used `std::atan2`, which
may differ in the last bit between platforms. `atan2_det()` (`field.hpp`,
beside `sin_det()` / `cos_det()`) is fdlibm's `atan` (its reduction table
and polynomial) with octant reflection, arithmetic only; it is within an
ulp of libm (checked on 200,000 points over 60 decades). Pseudonormals are
computed on the CPU and uploaded, so the GPU follows.

**The `0.0 / 0.0` guard in `closest_on_triangle()` is dropped**: not
reproducible. Triangles are FP32 positions whose differences and products
are exact in FP64, and a triangle with no area is left out when the BVH is
built, so the denominators (`|ab|²`, `|ac|²`, `|bc|²` and the barycentric
sum) didn't reach zero in 1.6 million queries against 200,000 near-collinear
slivers (scales 1 µm to 1 m, anywhere in the working space) or against
subnormal-height slivers. The shader is left as it is.

**Settings are in the keys.** `eval::cache_key()` takes the
`ProductSettings` and hashes them after the engine version, and
`proc::evaluate_graph()` hashes `inputs.settings` into every node's key.
The tolerance and working space change what generation accepts (minimum
feature sizes, containment), and L5's configurations will evaluate one
project under several engines; before, an engine with another tolerance
read the first one's disk cache and project caches.

**`kEngineVersion` 1 → 2**, once, for the three changes above. Saved
projects still load (`"engine"` in `project.json` is informational; the
checked-in samples keep `1` and load as before). Disk and project cache
entries written by engine 1 have other keys, and an entry whose JSON says
engine 1 is a miss (tested), so they regenerate.

**Graph output codes.** `proc.graph.output` is split: `proc.graph.no_output`
is unfinished (an empty or partly built graph), and
`proc.graph.multiple_outputs` (every output node, the second first) is an
error `edit()` refuses. Inspecting a node still ignores both, so a graph
read from a file with two outputs can be inspected.

**Curved surfaces in nodes.** `ConnectionInput::radius` is set for a
cylindrical (L2) patch, whose `origin` and `normal` are its axis.
`scatter.surface` turned every frame's z along that axis; it now points
radially away from the axis at each point. `input.surface`'s `plane` was
the plane square to the axis through its origin; for a curved surface it is
the signed distance to the cylinder (positive away from the axis), capped
far enough beyond the working space that the caps are never nearest, and
`normal` is documented as the axis. `children.delegate` refuses a curved
surface (generated children are boxes). Planar inputs hash as before.

**Thin-grid merge memory.** The merge sorted crossed cubes into waves
`i + 3j + 9k` with a counter per wave, so a long, thin grid paid for every
wave along it: on a 2 × 2 × 4M grid with a few blobs, 562 MB and 454 ms for
792 triangles. The plan's first option, giving stride 1 to the longest axis,
would break the bit-for-bit guarantee: a neighbour the serial order visits
first must be in an earlier wave, which forces the strides to grow with the
grid order's axis significance (z's stride above x's plus y's). The second
option is taken: when the waves outnumber the crossed cubes, the cubes'
`(wave, index)` pairs are sorted instead of counted. The same 2 × 2 × 4M
grid now takes 0.4 MB and 0.9 ms, and the mesh is the reference's on 1, 4
and 16 threads (tested). Dense grids keep the counting sort.

**The vs_proc / vs_gpu layout contract** is `field_layout.hpp`: where the
cylinder, lattice / TPMS and instances operations keep their numbers.
`FieldExpr::eval()`, the factories and the shader generator use it; the
generator `static_assert`s the `LatticeCell` and `TpmsSurface` ordinals its
shaders compare, the instances layout it spells out, and that its 64-entry
BVH stack holds `kMaxBvhDepth + 2`. The generated GLSL and buffers are
byte-identical to `main`'s (compared on a set of fields covering every
changed operation).

**1, 4 and 16 threads.** `eval::default_thread_count()` takes `VS_THREADS`
(1 to 1,024) before the hardware count for pools of size 0, and the
`linux-gcc-release` CI job runs the `vs_proc` and `vs_eval` suites again
with `VS_THREADS` 1, 4 and 16.

## Golden values

New: `libs/vs_proc/tests/test_distance_golden.cpp` hashes the BVH, the
pseudonormals and 6,913 samples each (signed and unsigned distance,
nearest triangle, feature and point) of a UV sphere, a split box at the
working space's corner (985 to 999.5 mm) and an open folded sheet, built
from literals and `sin_det()` / `cos_det()` and sampled at integer-generated
points. Before and after H3 (GCC 13, libstdc++):

| Hash | `main` | H3 |
| --- | --- | --- |
| sphere structure | `9a2626c9…` | `339953b8…` |
| sphere normals | `b180e880…` | `6478e533…` (atan2) |
| sphere samples | `2c055033…` | `d00d4255…` |
| corner structure | `8624b330…` | `c9774ccc…` |
| corner normals | `8ed4c410…` | unchanged (right angles) |
| corner samples | `6b9135fc…` | `c9eaa352…` |
| sheet structure | `ea809dd3…` | `1a288e04…` |
| sheet samples | `25265f63…` | unchanged |

Unchanged: `vs_core`'s hash goldens, every checked-in sample project, 3MF
and template, and every other suite's expected values (none of them hash
the leaf order or a pseudonormal's last bit). Cache keys and node keys
change through the engine version and the settings, as intended.

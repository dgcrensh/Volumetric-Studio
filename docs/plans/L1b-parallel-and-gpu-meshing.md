# L1b — Faster meshing: parallel surface nets, GPU where it pays (plan)

Nov 20, 2026 · follows L1 (L2 in the plan is curved connection surfaces, so this is L1b) · decision record (when built): `docs/decisions/0019-parallel-and-gpu-meshing.md`

## Goal

Make meshing a procedural volume (the boundary clip and surface nets) as
fast as sampling it, with **exactly the current mesh**: same FP64 vertices
before rounding, same triangles in the same order, same hashes and caches,
on any number of threads and with or without a GPU. As in L1, nothing is
approximated or quantised; where a faster path can't be proved identical,
the current code keeps doing the work.

## What we measured (Nov 20, 2026, Dallas's desktop)

NVIDIA GeForce RTX 2070 Super (driver 616.87), 12 CPU threads,
`bench-gyroid --size 100 --cell 0.5` (8.1 million samples, 9.6 million
triangles), with temporary timers inside `surface_nets()`:

| Stage | CPU (12 threads) | `--gpu` |
| --- | --- | --- |
| sample | 0.86 s | 1.59 s |
| clip (12 threads) | 0.35 s | 0.42 s |
| surface nets: vertices (1 thread) | 2.2 s | 2.2 s |
| surface nets: polygons (1 thread) | 6.5 s | 6.5 s |
| surface nets: merge per cube (1 thread) | 10.2 s | 10.2 s |
| rest of `mesh_contents()` (seams, containment moves, finish, topology) | 3.2 s | 3.0 s |
| engine containment check | 0.9 s | 1.0 s |
| **engine total** | **26.0 s** | **27.0 s** |

L1's self-check passes on this GPU (`vs_gpu_tests`: all 6 cases), so its
samples are exact here too.

Three things follow, and they shape this plan:

1. **All of surface nets runs on one thread.** Its three passes are
   18–19 s of the 26 s, and the machine has 12 threads idle meanwhile.
2. **The biggest pass, the per-cube merge, is sequential by nature.**
   Whether a collapse passes the link condition depends on collapses
   already made in neighbouring cubes, so the result depends on the order
   cubes are visited in (grid order today). A GPU can't run it as is and
   give the same mesh.
3. **Consumer GPUs are slow at FP64.** The 2070 Super runs doubles at
   1/32 of its float rate, so here L1's GPU sampling is *slower* than the
   12-thread CPU (1.59 s against 0.86 s). The L4 VM, with 4 slow vCPUs,
   showed the opposite. Surface nets' vertex positions are FP64 too, so
   the GPU only helps on machines where it is actually faster, and the
   software has to know which machine it is on.

So L1b does the exact CPU parallelism first (it helps every machine,
Linux included, and covers the merge), then moves the passes that a GPU
can do exactly (clip, vertices, polygons) onto it, and uses each device
only where it measured faster.

## Design

### Part A: exact parallel surface nets on the CPU

- **Vertices and polygons by z slabs.** Each pass splits the cubes (or
  grid points) into slabs of z slices on the `eval::ThreadPool`. Each slab
  writes its own arrays in grid order; a prefix sum over slab counts gives
  every slab its offset, and the slabs are concatenated in order. Vertex
  ids, `group_starts` and the triangle list come out exactly as the serial
  loop makes them, because every value is computed by the same code from
  the same samples, and only where it is stored changes. The dense
  `cell_of` index is filled by the vertex pass before the polygon pass
  reads it.
- **The merge by wavefronts.** A collapse in cube c reads and changes only
  the fans and neighbour sets of vertices in c and in cubes sharing a grid
  edge with it (a triangle joins tetrahedra round one grid edge, so its
  three vertices lie in mutually adjacent cubes). So cube c's result
  depends only on adjacent cubes visited before it. Give cube (i, j, k)
  the wave number t = i + 3j + 9k and run the waves in order, the cubes of
  one wave in parallel:
  - an adjacent cube visited earlier in grid order always has a smaller t
    (k one less: at least 9 − 3 − 1 less; same k, j one less: at least
    3 − 1 less; same row: 1 less), so every dependency the serial merge
    sees is honoured, in the same order;
  - two cubes in the same wave are at least 3 cubes apart, so they share
    no neighbouring cube and touch no vertex in common: no locks, no
    races.
  About 2,600 waves for a 200³ grid, each a `parallel_for` over the
  wave's crossed cubes (cubes listed per wave once, in grid order). Small
  waves run inline.
- **`Fans` made safe to share.** Growing a fan today moves it to the end
  of one shared array. Each worker gets its own overflow arena instead, so
  growing never reallocates what another worker reads. Fan order never
  affects a decision (neighbour lists are sorted, the two shared triangles'
  opposite corners are sorted), and compaction runs in vertex and triangle
  index order as before.
- **A reference to compare against.** The current serial code is kept,
  unchanged, as `surface_nets_reference()` (tests and the self-check only).

Expected on this desktop: surface nets from about 19 s to about 2.5 s,
the engine from 26 s to about 9 s. What remains is then mostly the rest of
`mesh_contents()` (3.2 s), which Part C looks at.

### Part B: clip, vertices and polygons on the GPU

- **A meshing hook in `vs_proc`**, like L1's sampling hook:
  `MeshingBackend` is offered (1) the clip, given the samples and the
  boundary's `TriangleDistance`, and (2) the unmerged surface net (the
  vertex sums and counts, `group_starts`, `cell_of` order and triangles
  before the merge). It takes them or declines; the CPU does whatever it
  declines. The merge always runs on the CPU (Part A).
- **Clip on the GPU** with L1's BVH walk, which already gives the CPU's
  signed distances bit for bit: `max(v, float(d + inset))` per sample,
  written to a second buffer (the generator keeps the unclipped samples).
  When the samples came from the GPU they are clipped where they are,
  without a round trip.
- **Vertices and polygons on the GPU** as three compute passes: classify
  each cube (crossed or not, how many tetrahedron vertices) and each grid
  point (how many crossed edges, how many triangles); exclusive prefix
  sums over those counts, so each cube and edge knows where its output
  goes in grid order; then write vertex sums (FP64, `precise`, summed in
  the CPU's edge order), and triangles, choosing each fan's apex with the
  same `double_area` comparisons (L1's correctly rounded `sqrt` makes
  `length()` identical). The output is the serial code's arrays, element
  for element. Slabs of z slices keep each dispatch well under Windows'
  2-second GPU timeout and check cancellation between them.
- **Memory.** The unmerged net is about six triangles per final one: for
  the 100 mm gyroid roughly 58 million triangles and 29 million vertex
  sums, about 1.4 GB to read back. The CPU path holds the same arrays
  today. Grids whose unmerged net wouldn't fit the device's memory go in
  z slabs, merged on the CPU in order.

### Part C: the slower device declines

- **Calibrate at install.** `gpu::install()` already runs a self-check; it
  also times a calibration grid (sampling, clip, unmerged net) on the GPU
  and on the CPU pool and enables each stage on the GPU only where the GPU
  was faster. On this desktop that will likely keep sampling on the CPU,
  which fixes L1's slowdown here; on the L4 it keeps it on the GPU.
  `status()` and the About box say which stages run where, and why.
  `vstudio-cli --gpu` gains `--gpu=force` for benchmarks and tests, to use
  the GPU whatever the calibration says.
- **Profile what's left.** After Part A, the rest of `mesh_contents()`
  (seam removal, the containment loop's distance queries, `finish()`,
  `mesh::analyse()`) and the engine's containment check are about half of
  the remaining time. Their per-vertex and per-triangle work is
  independent and goes on the pool in fixed chunks, the same way, where it
  isn't already; anything order-dependent stays serial.

### Safety net

- **Tests** compare `surface_nets()` with `surface_nets_reference()` for
  3,000 random grids (exact zeros, thin walls and tunnels, the tangled
  cubes of decision 0011), at 1, 2, 3 and 12 threads, and for the M12
  templates and the procedural sample projects; FP64 positions and
  triangle lists must be identical, not just the hashes.
- **The GPU self-check** gains a meshing battery: the same kind of grids,
  clipped and unmerged on both sides, every element compared. The GPU
  meshing stages are enabled only when it all matches.
- **Engine hashes**: the contents hashes of the procedural samples and
  `bench-gyroid`'s output hash stay what they are on `main`, with and
  without `--gpu`, at any thread count. No golden value moves.

## Acceptance

- `surface_nets()` gives exactly `surface_nets_reference()`'s mesh on the
  random battery, the templates and the samples, at every thread count
  tested, with and without the GPU.
- The engine's contents hashes for the procedural sample projects, and
  `bench-gyroid`'s output, are unchanged from `main`.
- `bench-gyroid --size 100 --cell 0.5` on this desktop: surface nets under
  3 s and the engine under 12 s (from 18 s and 26 s), CPU alone; the
  numbers with and without the GPU are recorded in decision 0019.
- With a GPU that is slower than the CPU at a stage, that stage stays on
  the CPU, and `status()` says so. Without a GPU everything works as
  before.

## Order of work

One branch, `l1b-parallel-meshing`, one PR:

1. `surface_nets_reference()` and the comparison tests, then Part A's slab
   passes and wavefront merge.
2. Part C's profiling and pool work on the rest of `mesh_contents()`.
3. The `MeshingBackend` hook and Part B in `vs_gpu`, with the meshing
   self-check.
4. Calibration, `status()` and the About box, `--gpu=force`.
5. Benchmarks on this desktop, decision 0019, plan status, CLAUDE.md.

Part A alone is most of the gain on this machine. If Part B's GPU passes
measure no faster than the parallel CPU here, it still lands (behind the
calibration) for machines like the L4 where they are.

## Out of scope

- The per-cube merge on the GPU: its order dependence makes an exact GPU
  version a research question, not a port.
- FP32 meshing, or any change to the mesh to make it parallel-friendly.
- Dual contouring, and a GPU containment check.
- GPU contexts outside Windows (EGL on Linux remains L1's follow-up).
- Tidy-up noticed while reading: `field.hpp`'s `SamplingBackend` comment
  has a vertical-tab byte where "`values`" should be; fixed in passing.

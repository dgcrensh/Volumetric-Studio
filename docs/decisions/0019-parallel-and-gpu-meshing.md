# 0019: Parallel and GPU meshing

Nov 20, 2026 · L1b · plan: `docs/plans/L1b-parallel-and-gpu-meshing.md`

## Questions

1. After L1, surface nets took 18 of the 26 s a 100 mm gyroid needs, all
   on one thread. Can it run in parallel and still give exactly the
   serial mesh, which every hash and cache depends on?
2. Its per-cube merge is order dependent: a collapse passes the link
   condition or not depending on collapses already made round it. How is
   that parallelised without changing a single decision?
3. Which parts can a GPU do exactly, and how does a machine with a GPU
   that is slow at FP64 avoid being slowed down by it (L1's sampling
   already was, on this desktop)?

## Decisions

**Two steps: the unmerged net, then the merge.** `surface_nets()` is now
`unmerged_net()` then `merge_net()`. The unmerged net (one vertex per
crossed tetrahedron, the fanned polygon round each crossed grid edge) is
independent per cube and per grid point, so it is made in z layers on the
pool, each into arrays of its own, joined in grid order with prefix sums.
Every value comes from the same expressions as before; only where it is
stored changes.

**The merge in waves.** A triangle's three vertices lie in cubes sharing
a grid edge, so a collapse in cube c reads and writes only the fans and
neighbour lists of vertices in c and in the cubes touching it. Cube c's
decisions therefore depend only on touching cubes visited before it.
Cube (i, j, k) goes in wave t = i + 3j + 9k, and the waves run in order:

- every touching cube the serial order visits first has a smaller t (one
  z layer down: at least 9 − 3 − 1 less; same layer, one row down: at
  least 3 − 1 less; same row: 1 less), so each cube sees exactly what the
  serial merge showed it;
- two cubes in the same wave are at least three cubes apart, so they
  share no touching cube, and run in parallel without locks.

Fans that outgrow their room move into a per-worker arena instead of the
end of one shared array, alive flags are bytes rather than `vector<bool>`
bits, and the fan initialisation and compaction run in fixed chunks (with
atomic counts, then each fan sorted back into triangle order). While
merging a cube, the surviving vertex's neighbour list is kept up to date
(N(u) ∪ N(x) without u and x after a collapse: the edges x–o of the two
removed triangles have other triangles, which become u–o) instead of
being rebuilt for every candidate.

**The serial code stays as the reference.** M11's surface nets is kept
unchanged as `surface_nets_reference()`. Tests compare the two element by
element (FP64 positions and triangles) on 3,000 small random grids (noise,
exact zeros, smooth blobs) at 1, 3 and 12 threads and without a pool, on
nine grids of 40³ to 64³ where waves really run on several threads, and
on a gyroid. All the existing golden hashes (engine output hashes of the
procedural samples and templates) are unchanged.

**The rest of `mesh_contents()` in parallel too.** `Builder::finish()`
writes the FP32 mesh in chunks. The final `mesh::analyse()`, a guard that
should never fire, was serial and took 1.7 s; `closed_manifold()` checks
the same thing per vertex on the pool (no degenerate triangle, each
half-edge once and its reverse once, each vertex's link one cycle) and
`analyse()` only runs when it says no, to describe what's wrong. A test
mutates a closed mesh 200 ways (holes, flips, doubles, degenerate and
moved corners, a pinched vertex) and checks it never passes what
`analyse()` rejects.

**The GPU takes the clip and the unmerged net, through a backend hook.**
`proc::MeshingBackend` is offered the clip and the unmerged net, as L1's
`SamplingBackend` is offered sampling; what it declines, the CPU does.
`vs_gpu` implements it:

- the clip samples `offset(boundary distance, −inset)` with L1's field
  shader (d − (−inset) is d + inset exactly) and takes the max on the
  CPU;
- the unmerged net is four compute passes over the padded grid: each
  cube's crossed-tetrahedron mask; each crossed tetrahedron's crossing sum
  and count at its cube's first vertex; each grid point's triangle count;
  each point's fans at its first triangle. The first vertex and first
  triangle are exclusive sums computed on the CPU between passes, so
  vertex and triangle order are the CPU's. Positions for the fan apex use
  L1's correctly rounded square root.

The merge stays on the CPU: its order dependence makes an exact GPU
version a research question.

One trap: NVIDIA's compiler gave a different fan apex when the polygon's
positions were kept in a local `dvec3[6]` array indexed at run time,
although the same arithmetic on scalars matched the CPU to the bit (a
regular hexagon's six candidate apexes differ by a few ulps). The shader
recomputes each position from the vertex buffer instead. The meshing
self-check (twelve random grids, a gyroid clipped to a ball and to a box)
caught it on the first run.

**Each stage goes to whichever device was faster here.** After the
checks, `gpu::install()` times sampling, clip and unmerged net on a
half-million-sample gyroid infill in a ball, on the GPU (the better of
two runs, so shader compiles don't count) and on every CPU thread, and
enables each stage on the GPU only where it won. `Status` says which
stage runs where and gives the timings; the CLI prints them and the
About box shows them. `InstallOptions::force` (`vstudio-cli
--gpu=force`) uses the GPU for every stage that matched, faster or not:
tests use it, since they check exactness rather than speed. Checks and
calibration together take about 3 s on this desktop (the app does it on
a background thread at start; the CPU works meanwhile).

## Results (Dallas's desktop: i7-10750H, 6 cores / 12 threads; RTX 2070 Super, driver 616.87)

`bench-gyroid --size 100 --cell 0.5` (8.1 million samples, 9.6 million
triangles, the same 415.9 g part as before):

| Stage | Before (L1) | L1b, CPU | L1b, `--gpu=force` |
| --- | --- | --- | --- |
| sample | 0.86 s | 0.9–1.0 s | 1.0 s |
| clip | 0.35 s | 0.35–0.40 s | 0.25 s |
| unmerged net | 8.7 s | 1.8–1.9 s | 3.9 s |
| merge | 10.2 s | 3.0–3.3 s | 3.1 s |
| rest of `mesh_contents()` | 3.2 s | 1.1–1.3 s | 1.3 s |
| **engine** | **26.0 s** | **10.8 s** | 13.2 s |

With `--threads 4` the engine takes 15.4 s. Plain `--gpu` calibrates
(sampling GPU 248 ms against CPU 135 ms, clip 245 against 116, unmerged
net 98 against 22) and keeps every stage on the CPU here: the 2070 Super
runs doubles at 1/32 of its float rate, and six fast cores beat it. Its
engine time is the CPU's. Surface nets as a whole (unmerged net and
merge) came down from 18–19 s to about 5 s, short of the plan's 3 s
estimate, which assumed 12 threads scale like 12 cores; this laptop chip
has six, and the merge is bound by memory traffic. The engine target
(under 12 s) holds.

On the L4 VM (4 slow vCPUs) the balance is the other way for sampling
(L1: 2.8x faster on the GPU), and calibration should hand it sampling and
probably the clip; that is to be measured when the VM is next used.

## Consequences

- Every mesh, hash and cache key is unchanged; no golden value moved.
- `surface_nets()`, `mesh_contents()`, `inspect`'s preview and the
  `convert.field_to_mesh` node all take the pool.
- `bench-gyroid` prints the unmerged net and the merge separately.
- Changing the meshing now means changing `unmerged_net_cpu()`, the
  reference and `meshing.cpp` together; the tests and the self-check say
  when they disagree.
- Not done: the merge on the GPU, the containment loop's distance queries
  on the GPU (0.9 s of the remaining time), EGL on Linux.

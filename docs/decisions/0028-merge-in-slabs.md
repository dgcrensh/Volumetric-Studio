# 0028: The merge in slabs, and the unmerged net filled in place

Nov 21, 2026 · follows H5 (decision 0027) · before H6 and H7

## Context

Decision 0027 left the merge as the peak of procedural meshing: on
`bench-gyroid --size 100 --cell 0.5 --threads 4` (Linux) the unmerged
net's triangles (12 bytes each, 42.5M of them), its vertex sums (24 bytes
a vertex, 21.2M) and the merger's fans (12 bytes a triangle plus 16 bytes
a vertex of run pointers, sizes and room), alive flags and 32-bit counts
were all live at once: 2,213 MB for the merge stage, 2,426 MB for the
engine. 0027 thought streaming the merge would change the order the
reference walks in. It doesn't have to: the constraint is that the mesh
stays `surface_nets_reference()`'s bit for bit, on any thread count and
compiler, with no golden hash changing.

## Candidates

1. **Slim the global merger.** Keep one set of fans for the whole net but
   drop what can live in the net's own arrays: counts stay bytes (a
   merged vertex sums at most 24 crossings, four in each of a cube's six
   tetrahedra) and mark a dead vertex by 0, a dead triangle is marked in
   place, and the compaction writes into the front of the sums and
   triangles instead of new arrays.
2. **Merge in slabs over the whole net.** Go up the grid a slab of cube
   layers at a time. A collapse in a cube touches its own vertices and the
   fans of vertices in the cubes round it, and every cube's touching
   neighbours below it come first in the serial order, so slabs one after
   another give the serial result. A slab needs the fans of its own
   layers and one layer each side only; behind it, vertices and
   triangles are final and are compacted in place (1's tricks). The
   `UnmergedNet` and `MeshingBackend` interfaces stay as they are.
3. **Fuse the net and the merge.** Make the unmerged net slab by slab and
   merge each slab right behind it, so the whole net never exists. Needs
   `MeshingBackend::unmerged_net()` to work per slab (the GPU's L1b
   passes in vs_gpu), which only the desktop can test.

1 and 2 exclude each other (one set of fans or a window of them); 3 is 2
plus a different producer.

## Measurements

`vstudio-cli bench-gyroid --size 100 --cell 0.5`, 4 threads unless said,
on a 4-core cloud container (Ubuntu 24.04, GCC 13.3, release). `bench-gyroid`
now prints each stage's own peak (reset between stages) and a digest of
the merged and the final mesh; every variant below printed the same
digests as `main` (`e5cdb4b8…` merged, `402f515f…` final). Times on this
machine vary by about ±0.5 s between runs.

| Variant | Unmerged net | Merge | Engine peak | Merge time |
| --- | --- | --- | --- | --- |
| `main` (H5) | 1,774 MB | 2,210 MB | 2,426 MB | 5.0 s |
| 1: slim global merger (one slab) | 1,163 MB | 2,306 MB | 2,464 MB | 5.4 s |
| 2: slabs of 16 layers | 1,775 MB | 1,221 MB | 1,893 MB | 5.9 s |
| 2 + net counted, then filled in place | 1,163 MB | 1,222 MB | 1,283 MB | 5.7 s |
| … + rows in waves (**chosen**) | 1,163 MB | 1,202 MB | 1,286 MB | 3.8 s |

- **1 doesn't pay.** The fans for the whole net dominate, and copying out
  the kept part at the end costs what the flags saved.
- **2 cuts the merge to the net it's given plus about 50 MB**, so the
  unmerged net's own construction (each z layer's arrays and the joined
  arrays both alive, 1,775 MB) became the peak. Counting each layer
  first and writing it straight into the net (vertices; then triangles,
  whose polygon count needs only the samples and the cube index), and
  finding a tetrahedron's vertex from its cube's first vertex and a byte
  of crossed tetrahedra instead of six 32-bit ids per cube, brings that to
  the net's own size (1,163 MB) and makes it faster (2.3 s to 2.1 s).
- **Slab size** (cube waves, before rows): 8 layers 1,213 MB, 16 1,222 MB,
  32 1,302 MB, 64 1,497 MB, all 2,306 MB; the merge took 6.6, 5.7, 5.6,
  5.2 and 5.4 s. 16 (`kMergeSlabLayers`) is past the knee.
- **Rows in waves.** In a 16-layer slab, L1b's cube waves `i + 3j + 9k`
  run about 935 waves of a few hundred scattered cubes, and four threads
  were no faster than one (4.5 s of waves either way, cache misses). Whole
  x rows as tasks, row `(j, k)` in wave `j + 3k`, keep each task's memory
  together: waves took 1.9 s. The argument is L1b's one level up: the
  weights must satisfy `c > 2b` for every touching row the serial order
  visits first to be in an earlier wave, and two rows of one wave are
  three rows or layers apart, so they touch nothing in common.
- **3 is bounded by what comes after the merge.** With 2 in place, the
  phases of `mesh_contents()` inside the engine peaked at: unmerged net
  1,210 MB, merge 1,309 MB, the closed-manifold check 1,102 MB (the
  builder's arrays are still alive), finish 840 MB, containment moves
  765 MB; the engine's containment check in the bench peaks at 950 MB.
  Removing the net entirely could take the engine from about 1,290 MB to
  about 1,100 MB (−15 %), not more, until those stages shrink too, and
  it needs the backend interface changed and verified on the GPU. Not
  built; see "Later".

Other thread counts and a larger grid, `main` → chosen:

| Run | Engine peak | Engine time | Merge time |
| --- | --- | --- | --- |
| 100 mm, 1 thread | 2,512 → 1,231 MB | 36.2 → 28.0 s | 14.0 → 7.5 s |
| 100 mm, 4 threads | 2,426 → 1,286 MB | 13.5 → 11.7 s | 5.0 → 3.8 s |
| 100 mm, 16 threads (on 4 cores) | 3,724 → 1,332 MB | 13.7 → 12.3 s | 5.7 → 4.0 s |
| 120 mm (241³, 73M unmerged triangles), 4 threads | 3,854 → 2,141 MB | 22.6 → 19.4 s | 8.8 → 6.4 s |

One thread gains most: a pool of one used to run the waves in their
cache-hostile order; now it runs the cubes in grid order.

## Decision

Take 2 with the net filled in place and rows in waves.

- `merge_net(net, grid, pool, cancel, slab_layers = kMergeSlabLayers)`
  (16). For each slab of layers `[k0, k1)` starting at the next layer with
  crossed cubes, it builds `Fans` for vertex layers `[k0 - 1, k1]` from
  the triangles that can touch them (a cursor range: a grid point layer's
  triangles have vertices in its layer and the one below), merges the
  slab's cubes (in grid order alone, or rows in waves on a pool), then
  compacts every vertex layer up to two below the next crossed layer and
  the triangles that use only those, in place, and keeps the old-to-new
  vertex index only for the layers later triangles can still reach. The
  merge never depends on the order of a fan's run (it sorts what it
  reads), so rebuilding fans from the live triangles changes nothing.
  Slabs skip layers with no crossings, so memory still follows the
  crossings on H3's long thin grid.
- `unmerged_net_cpu()` counts each layer's vertices, cubes and triangles,
  then fills the net at its offsets; the net it returns is the same
  (`UnmergedNet ==`), so `MeshingBackend` and vs_gpu's equality checks
  are untouched.
- The merged mesh is copied out exactly sized, one array at a time, and
  the net is freed as it goes.

## Later

- The next peaks are the unmerged net itself (1.16 GB here) and, after
  the merge, `closed_manifold()` with the builder's arrays still alive
  (1.10 GB) and the containment checks (0.95 GB). Fusing the net and the
  merge (3) is worth doing only together with those, and with a
  per-slab `MeshingBackend::unmerged_net()` tested on the GPU.
- `mesh::Mesh` stores a 16-byte volume id per triangle and a 16-byte seam
  id per vertex: 230 MB of the 400 MB final mesh here.

## Tests

`libs/vs_proc/tests/test_parallel_mesher.cpp`: merges in slabs of 1, 2,
3, 5, 16 and 1,000 layers on 1,000 small random grids and six larger ones,
serially and on 3 and 12 threads, each equal to the reference element by
element; the net and the merged mesh hold no spare capacity. The existing
cases (random grids, waves in parallel, the gyroid, the long thin grid,
cancellation) pass unchanged, as do every suite's golden hashes. CI runs
the vs_proc and vs_eval suites on 1, 4 and 16 threads.

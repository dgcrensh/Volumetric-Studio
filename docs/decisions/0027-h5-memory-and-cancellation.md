# 0027: H5, memory, cancellation and performance

Nov 21, 2026 · plan `docs/plans/H-hardening.md` (PR 28), phase H5

## Context

H5 bounds what grows over a long session (blobs copied per undo step,
caches that only grow, an unlimited undo stack), makes procedural meshing
stop inside the libraries when it is cancelled, takes the largest
avoidable allocations out of the mesher's peak, and fixes the Qt-free app
bugs that `vs_app_core_tests` can cover. Golden hashes, sample projects,
meshes and every other suite's expected values are unchanged: the mesher
still produces `surface_nets_reference()`'s mesh bit for bit, and nothing
here changes a cache key or `kEngineVersion`.

## Decisions

### Shared blobs

`model::BlobStore` maps paths to `BlobPtr` (`std::shared_ptr<const Blob>`).
Copying a `Project` (each undo step, preview tick, drag frame and import)
now copies pointers. `Project::blob(path)` returns the bytes or null;
`operator==` compares the bytes, shared or not, so equality and
`content_hash()` mean what they did. `add_blob()` keeps an existing blob
under the same path (paths are content hashes), so a re-import shares the
first copy. The `.vsproj` reader and writer are unchanged on disk.

### Byte budgets, not entry counts

- **`eval::ContentsCache`** keeps the memory tier as an LRU under
  `EngineOptions::memory_cache_bytes` (default 2 GiB). An entry's size is
  `contents_bytes()`: the mesh's arrays by capacity, `ExtraContents::
  memory_bytes()` (vs_proc's `FieldContents` counts its samples and
  stitched surfaces), and a flat 256 KiB for a B-rep solid or a parametric
  rebuild. OCCT gives no cheap size for a shape, and a solid's
  `write_brep()` text is a poor proxy for its in-memory size, so a fixed
  estimate it is; B-rep contents are small next to meshes. The newest
  entry is never evicted, so a single result larger than the budget is
  still kept for its volume. The disk tier is unbounded, as before.
- **`proc::NodeCache`** is an LRU under 1 GiB (`NodeCache::kDefaultBudget`)
  in place of the 4,096-entry clear. `value_bytes()` sizes a value: a
  mesh by its arrays, a field by its distinct expression nodes (a subtree
  shared twice counts once), a boundary distance by its positions plus
  about 160 bytes a triangle for the BVH and pseudonormals.
- **`History`** keeps at most 200 undo steps (`set_undo_limit()`),
  dropping the oldest; with shared blobs a step costs a document, not a
  project's meshes.

### Cancellation in the libraries

`mesh_contents()`, `clip_to_boundary()`, `surface_nets()`,
`unmerged_net()`, `merge_net()` and their `_cpu` halves take an optional
`const eval::CancellationToken*`. They check it between stages, per z
layer, per merge wave (every 4,096 cubes on the serial path), every 65,536
triangles in the stitch and per round in the containment push, and return
an empty result (`ContentsMesh::cancelled`). `NodeContext::cancelled()`
reaches `convert.field_to_mesh` and the `scatter.*` loops;
`evaluate_graph()` sets `GraphEvaluation::cancelled` and never caches a
node that was stopped. The generator turns it into
`GeneratedContents::cancelled`, so the engine leaves the volume Stale with
nothing cached and nothing written to `last_valid_`. The test cancels a
40 mm gyroid at 0.3 mm a fifth, half and four fifths of the way through
and requires a return within 250 ms (five times that under ASan); a
backend that cancels exactly between the clip and the nets checks the
engine side. Wiring `Session` to the token is H7, as planned.

### Peak RAM

Measured with `vstudio-cli bench-gyroid --size 100 --cell 0.5 --threads 4`
on Linux (peak resident set, `VmHWM`). `bench-gyroid` now prints the peak
of the staged run and of the engine run separately, resetting it in
between, and frees each stage's result before the next: on `main` the
bench held them all, which made the stages look 800 MB heavier than the
library is.

| | stages | engine |
| --- | --- | --- |
| `main` (same bench) | 2,679 MB | 2,833 MB |
| H5 | 2,242–2,289 MB | 2,394–2,396 MB |

About 15 % (440 MB) off the engine's peak. What changed:

- the unmerged net no longer stores a position per vertex: the merge
  reads the vertex sums and counts it already had (`Centres`);
- `merge_net()` frees the per-cube counts once the merger is built and
  the cube list once the waves are ordered; the merger's counts are 32-bit;
- `Fans` releases its run pointers before `finish()`;
- `Builder::stitch()` maps only candidate triangles (those touching the
  patch's rim) to their owners, adding the triangles of the chosen piece
  as it grows, and builds fan lists only for pinched vertices. On the
  plan's 10M-triangle gyroid with seams this is the 1.5–2 GB the plan
  estimated (two maps of a few hundred bytes per triangle); the bench has
  no seams, so the number above doesn't show it.
- `mesh::weld()` takes `std::span<const Mesh* const>` (`combine()` no
  longer copies every volume's mesh) and indexes only edges and vertices
  that are defects.

**The merge is now the peak** (about 2.05 GB as it starts, 2.18 GB at
its end on the bench above): the unmerged triangles (12 bytes each,
about 510 MB here), the vertex sums (24 bytes a vertex) and the fans
(12 bytes a triangle plus run pointers) are all live at once. Streaming
the merge by wave, or merging per z slab, would cut that further; both
change the order the reference walks in, so they are for a later plan
with a new reference. The merge stays exact.

### CPU

- `faces_of()` reads a planar face's plane with `pln.Location()`
  (`kernel::face_surface_plane()`), with no area integration.
- A multi-profile extrude or revolve unites its pieces in one
  multi-tool fuzzy boolean.
- `VariableTable::evaluate()` builds its dependency graph once and orders
  it with a min-heap Kahn's algorithm (the same order as before).
- `SampledField::hash()` feeds canonical floats to BLAKE3 in 16 KiB
  blocks (the same bytes, so the same hash); the binary STL reader
  reserves, and the 3MF writer formats into a reserved buffer.

### Qt-free app fixes

- **Sketch editor:** a horizontal or vertical distance or an angle
  (`signed_dimension()`) drags through zero; lengths, radii and diameters
  stop at `kMinDraggedDimension` (0.01 mm), in `sketch_mode.cpp` too. A
  click that makes nothing (zero radius, a line onto its own start, an arc
  whose end is its start) leaves no undo step and keeps the redo stack.
- **Ship:** `ShipCheck` keeps the weld it made (`combined`) and counts
  weld errors apart; `ok_for(ShipTarget::Step)` ignores them, and
  `summary(check, Step)` says the mesh doesn't weld but STEP can go.
  `export_mesh(evaluation, combined, format)` writes from that weld.
- **`new_errors()`** compares code and subjects (kind and id, the label
  only for a null id), not messages that may quote a changed value.
- **`connect_ports(..., a_is_output)`** takes the end a wire was dragged
  from, for nodes whose input and output share a name.
- **Scene colours** follow `volume_tree_order()` (the rows' order), so a
  nested document's swatches and viewport agree.
- **`make_placeholder()`** refuses a composite with children
  (`app.swap.has_children`), as `swap_contents()` does.

## Departures from the plan

- **Qt files changed but not compiled here.** `main_window.cpp` (STEP export
  past a weld error, export reusing Validate's weld when the snapshot is
  the same), `node_editor.cpp` (passing the drag direction) and
  `sketch_mode.cpp` (no clamp on signed drags) are small, but only the
  `windows-msvc-app` build compiles them: they are verified first thing in
  H6, with H1's app split. The colour index in `main_window.cpp:132`
  already used the row order; the plan's `main_window.cpp:844` item stays
  with H7.
- **No byte cap on the disk cache.** The plan bounds memory; the disk
  tier is per project and cleared with it.

## Tests

- `libs/vs_proc/tests/test_memory_and_cancel.cpp`: the node cache's LRU,
  `value_bytes()`, every stage stopping on a cancelled token, mid-run
  cancellation within 250 ms, and a job cancelled in meshing left
  uncached.
- `libs/vs_eval/tests/test_hardening.cpp`: the contents cache's LRU, and an
  engine with a one-byte budget giving the same results as an unbounded one.
- `libs/vs_model/tests/test_project.cpp`: copies of a project share blobs
  and still compare and hash by bytes.
- `vs_app_core_tests`: 50 edits share blob pointers, the undo limit,
  `new_errors()` by code and subjects, same-name port wiring both ways,
  STEP with an unweldable mesh and the weld reused, composite colours in
  tree order, the placeholder refusal, signed drags through zero and
  zero-size clicks.

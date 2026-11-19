# 0009: The evaluation engine

Nov 19, 2026 · M9

## Questions

1. `model::build()` (M5) builds the layout and every volume's contents in
   one pass. How does the engine regenerate only what changed, in
   parallel, without duplicating vs_model's builder?
2. What exactly goes into a volume's cache key, and what about static
   volumes, which "never regenerate"?
3. Links feed one volume's published output into a layout variable or
   another volume's parameter. How are they scheduled?
4. What can safely run on worker threads, given OCCT?
5. How are results made independent of the thread count?
6. What does "failed", "stale" and "cancelled" leave behind?
7. What goes in the on-disk cache?

## Decisions

**Split the build in two.** `model::build_layout()` rebuilds the layout,
boundaries (keep-outs cut), connection surfaces and volume parameters;
`model::build_contents()` builds one volume's contents from that result
and only reads it, so calls for different volumes can run at once.
`build()` is now the two halves in a row and gives the same results as
before (every earlier test passes unchanged). Parameters moved from the
contents half to the layout half, because they are layout expressions.

**The cache key hashes what reaches the contents and nothing else**
(`cache.hpp`): engine version, kind, the content definition (blob path,
which is a content hash, and format; the feature tree's JSON; the graph
text), the boundary's exact shape (`kernel::shape_hash()`, BLAKE3 of
OCCT's BRep text without triangulations) and its named faces, each
joining surface's built patch, frame, gap, minimum wall, thread, hole
centres and seam spacing, the evaluated parameters, the seed and the
outputs it measures. Names aren't inputs, so renaming a volume doesn't
regenerate it. A layout change that doesn't move a boundary doesn't
regenerate the volume either, because the key holds the boundary's shape
rather than the features that made it. **Static volumes leave parameters
and seed out**: a file can't change with them, so the bracket's Upright,
whose `load` parameter is linked to the Base's mass, isn't re-read when
the Base changes. The boundary stays in, because containment depends on
it.

**Links are scheduled in waves.** A volume waits for every volume its
contents depend on (the M5 dependency graph, which has no loops). Before
each wave the layout is rebuilt with the link values found so far, but
only when they changed, and a target whose source has no value yet keeps
its own expression. In the chain A → B the first evaluation runs A (and
every independent volume), then B. Published outputs are versioned by the
key of the contents that made them (the plan's open question): B's key
holds the linked value, so B regenerates exactly when A's output changes.

**Contents generate in parallel; containment runs on the calling
thread.** A parametric rebuild sees its boundary and surfaces only as
planes and values (`ExternalInputs`), and STEP and IGES reading is
already serialised in vs_kernel, so generations share no OCCT shapes.
Containment cuts the contents with the boundary, and OCCT's booleans may
add p-curves or raise tolerances on their arguments unless run in
non-destructive mode, so the engine runs them one at a time after each
wave rather than change every boolean in vs_kernel. They are a small part
of the cost.

**Determinism by construction.** Each volume generates on one thread and
writes only its own slot; the pool's `parallel_reduce()` folds fixed-size
chunks in chunk order, so sums (a static mesh's volume, area and centre
of gravity) are bit-identical with 1, 4 or 16 threads. A call from inside
a pool task runs inline, which changes where work runs, never the
result. The output hash of a volume covers its status, its exact shape
(BRep text) or mesh, and its outputs' values; `check_determinism`
generates each regenerating volume twice and compares them.

**States.** A volume's contents that fail to generate, or aren't
contained, make it Failed: it shows its last valid contents marked stale
and blocks the volumes downstream of it (Stale, with `eval.blocked`),
while everything else carries on. Failed results are cached like good
ones, so the same failing inputs don't regenerate again. A cancelled
evaluation starts nothing new: finished volumes are kept and cached
(they are valid for their keys), the rest keep their previous contents as
Stale, and the next evaluation simply finds the finished ones in the
cache. The kernel can't be interrupted mid-operation, so a volume already
generating finishes. The preview tier is the same evaluation without
generating: boundaries at once, cached contents where the key is known.

**The disk cache holds clean B-rep contents only**, as
`cache/<key>.brep` (BRep text) and `cache/<key>.json` (outputs,
diagnostics, output hash), written atomically and read as a miss when
missing, damaged or from another engine version. Meshes are cheap to read
from their blobs again, and failures are cheap to recompute once per
session, so neither goes to disk. A parametric volume's feature rebuild
result stays in memory. `save_project()` already leaves a folder
project's `cache/` alone and zips never include it, so caches stay out of
the project's content hash.

**Mesh files belong to the engine.** vs_model left STL, 3MF and OBJ
contents Stale; the engine reads them (vs_mesh), measures their outputs and
checks containment with a mesh view. STEP and IGES files import as static
volumes boxed by their bounds (`eval::import_volume()`), or into an
existing volume (`import_contents()`), where containment then matters.

**Routing.** `eval.*` diagnostics whose first subject is a volume route to
its contents, like `model.contents.*`.

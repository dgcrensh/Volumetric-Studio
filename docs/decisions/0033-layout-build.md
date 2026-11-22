# 0033: Build layouts without redundant checks, on every thread

Nov 22, 2026 · follows the box layout checks (decision 0032)

## Context

After 0032, a preview of the 10,000-cell rack took 23 s, and building the
layout (`model::build_layout()`, inside the engine's "layout and checks"
phase) was its largest part. Timers around each step of `build_layout()`
on the 10,000-cell rack (Linux cloud session, 4 vCPUs, `linux-gcc-release`):

| step | 1,000 cells | 10,000 cells | growth |
| --- | ---: | ---: | ---: |
| composite layout rebuild (20,000 datum points, 19,800 planes) | 7 ms | 1,711 ms | 240x |
| boundaries (`make_box()` per child) | 512 ms | 5,783 ms | 11x |
| connection surfaces (`make_face()` per surface) | 577 ms | 6,432 ms | 11x |
| root level's surface loop | 2 ms | 158 ms | 88x |
| keep-outs and named faces | 21 ms | 258 ms | 12x |
| **build_layout()** | 1,122 ms | 14,351 ms | |

Two parts were quadratic; the rest was linear, but expensive per item.

- **`feature::rebuild()`** named each feature with `FeatureTree::subject()`,
  which searches the list for the feature and again to count its kind:
  quadratic in features, and a generating composite's layout holds two
  datum points per child and a plane per surface.
- **`build_level()`** picked each level's surfaces with
  `Document::find_volume()` and `level_of()` (list searches), and named
  subjects with `Document::*_subject()` (another search each).
- **Per item**, perf put about nine tenths of `make_box()` and three fifths
  of `make_face()` in `check_validity()` (`BRepCheck_Analyzer`): 0.54 ms
  per box and 0.26 ms per rectangle, one thread.

## The quadratic parts (no alternatives needed)

- `rebuild()` makes the tree's subjects (`FeatureTree::subjects()`, one
  pass) and an id → index map once, and each feature's build reads them.
  Labels are what `subject()` gives, a repeated id taking its first
  feature's, as before.
- The layout half of `DocumentBuilder` builds a `DocumentIndex` (0031) once
  and asks it for volumes, subjects, children, levels and joins. The
  contents half (one volume at a time, possibly on many threads) keeps the
  list searches, so it doesn't build an index per volume.

The composite rebuild went from 1,711 ms to 38 ms, and the surfaces from
6.4 s to 5.1 s.

## Candidates for the per-item cost

1. **Skip checks that can't fail.** `BRepPrimAPI_MakeBox` with finite
   sides no shorter than the minimum feature size (already refused
   otherwise) always makes a valid solid; a planar face bounded by a
   strictly convex polygon with no holes can't cross itself or have its
   holes touch, which is what the check is there to catch. `make_box()`
   returns without `check_validity()`; `make_face()` skips it when the
   outer loop is all lines turning the same way at every corner, once
   round, and there are no holes. Anything else is checked as before.
2. **Build in parallel.** `build_layout(doc, settings, parallel)` takes a
   `ParallelFor` (the engine's pool) and builds a level's boundaries and
   its surfaces on all threads, each into its own slot, so the result is
   the same on any thread count. What the builds read (the volume map,
   the level's rebuilt features) is indexed before they start. Boundaries
   that unite several layout tools run a boolean, which may fill caches on
   shapes other builds read (H2), so those stay on the calling thread.
3. **Both.** They don't exclude each other; the question was whether one
   alone was enough.

A fourth, **no OCCT shapes for box boundaries** (corners only, B-reps made
when something asks), was left out: every reader of `VolumeBuild::boundary`
would change, and the shapes are still needed for keys and checks.

## Experiment

`build_layout()` time on the rack, with the quadratic fixes in all four
(two runs each, ms; 4 threads):

| | 1,000 cells | 10,000 cells |
| --- | ---: | ---: |
| checks, one thread (fixes only) | 1,053 / 1,228 | 12,046 / 11,281 |
| checks, parallel | 426 / 480 | 4,204 / 4,453 |
| no redundant checks, one thread | 306 / 292 | 2,889 / 3,093 |
| **no redundant checks, parallel** | **107 / 103** | **1,068 / 1,084** |

Skipping the checks saves about three quarters on its own, and the
parallel build divides what is left by about the thread count; together
they take the 10,000-cell build from 14.4 s to 1.1 s.

## Decision

All three changes: the quadratic fixes, skipping checks that can't fail,
and the parallel build, which the engine always uses (`layout_of()`, with
or without layout reuse). `build_layout(doc, settings)` still builds on
the calling thread.

No golden hash, cache key or saved fixture changes: the shapes are built
by the same calls, and only the reading of them is skipped or moved.

## Tests

- `test_primitives.cpp`: 300 random boxes (sides from the minimum feature
  size to 100 mm, anywhere in the working space) and 300 random convex
  polygons (3 to 12 corners on an ellipse, any plane, either winding)
  pass `check_validity()`; a bow tie is still refused.
- `test_parallel_layout.cpp`: on the bracket, curved and nested fixtures,
  a generated 6 × 4 grid and a grid with a broken boundary and a
  too-small surface, `build_layout()` on four threads and in reverse order
  gives exactly the serial build: boundary and patch shape hashes, named
  faces, parameters, frames, diagnostics and their order.
- The vs_eval and vs_validate suites pass with `VS_THREADS` 1 and 16.

## Results

`bench-rack --cells 10,100,1000,10000 --threads 4`
(`docs/benchmarks/bench-rack-2026-11-22-linux-layout.csv`):

| cells | preview ms, 0032 | preview ms, now | again ms | peak MB |
| ---: | ---: | ---: | ---: | ---: |
| 10 | 22 | 27 | 3.0 | 20 |
| 100 | 196 | 122 | 24 | 26 |
| 1,000 | 1,894 | 1,226 | 269 | 81 |
| 10,000 | 23,015 | 13,072 | 4,442 | 636 |

At 10,000 cells, layout and checks take 6.6 s (from 17.1 s).

With one thread (`--threads 1`), 10,000 cells take 19.4 s (layout and
checks 12.5 s). Peak memory is unchanged. Time
now grows linearly from 1,000 to 10,000 cells.

## Follow-ups

- Of "layout and checks" at 10,000 cells, the layout build is now about
  1.1 s and `validate_layout()` about 5.4 s: each chunk's deep copies and
  the connection rules' face measurements are the next target.
- Keys and cache (3.3 s) and expand and check (1.6 s), dependencies
  (1.5 s) are now the other large phases.
- A rectangle's face could be made from its plane directly instead of
  finding the plane from the wire (0.1 ms of each surface), but the
  B-rep text, and so the cache keys, would change.

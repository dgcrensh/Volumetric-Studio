# 0034: Check layouts without idle copies, integrals and samples

Nov 22, 2026 · follows the layout build (decision 0033)

## Context

After 0033, a preview of the 10,000-cell rack took 13.1 s, and
`validate_layout()` was its largest single step: 4.8 s on 4 threads
(6.8 s on one). The goal here was speed with **no robustness trade-off**:
every report identical to today's, to the last bit, not merely within the
tolerance as 0032 allowed.

Timers round each step (`vs_validate_tests "[.scaling]"`, 100 to 10,000
cells, CPU seconds summed over the sizes, `linux-gcc-release`, 4 vCPUs):

| step | one thread | four threads |
| --- | ---: | ---: |
| prepare: each boundary's box, and whether it is exactly its box (serial) | 4.1 | 4.6 |
| pairs: deep copies of boundaries | 0 | 5.6 |
| pairs: the checks themselves | 0.1 | 0.1 |
| connection rules: tessellating each patch for sample points | 4.9 | 10.2 |
| connection rules: `rectangle_of()` | 2.0 | 2.8 |

Three things stood out.

- **Idle deep copies.** On several threads each chunk of pairs deep-copied
  every boundary it met (OCCT's distance tools may fill caches, H2), but
  a pair of boxes is checked by arithmetic (0032) and never reads them. At
  10,000 cells every copy was wasted, and copying scaled badly across
  threads.
- **Area integrals for a normal.** Deciding whether a boundary is a box
  and whether a patch is a rectangle read `kernel::face_plane()`, whose
  origin is the face's centre of area (an integral), and then used only
  its normal. The six side names of every box were also worked out up
  front, another integral per face, though only a reported contact uses them.
- **Samples that can't matter.** Every patch was tessellated so its points
  could be tested for lying inside a joined boundary, though a rectangle
  lying on the box's side can't reach inside it farther than the
  tolerance.

## Candidates

All five keep the same computations or prove a computation's answer in
advance; none changes what is compared or with which tolerance.

1. **Lazy copies.** A chunk copies a boundary the first time a pair going
   through OCCT needs it. Pairs of boxes copy nothing.
2. **Parallel preparation.** Each boundary's box and box test run in fixed
   chunks of 64 volumes on the engine's pool, each into its own slot. A
   task reads only its own volumes' boundaries, and only their geometry
   (bounding boxes, plane normals); no classifier or distance tool, so
   nothing that fills caches (H2).
3. **Names when reported.** A box keeps the index of the face on each side;
   the names are worked out, on the calling thread, only for the face
   contacts the box path reports, by the same `face_label()`.
4. **Normals without integrals.** The box and rectangle tests read
   `kernel::face_surface_plane()`, whose normal is computed by the same
   lines as `face_plane()`'s, without the centre of area.
5. **Skip settled samples.** For a rectangle on a box boundary, every
   sampled point's depth inside the box is at most its distance to either
   side across the patch's normal. When the patch lies within the
   tolerance of one of those sides (less `kSampleSlack`, 1e-9 mm, for
   the rounding of putting a point on the plane), no point can be deeper
   than the tolerance, so `deepest_inside_box()` of the samples would be
   0: the patch isn't tessellated. Anything else is sampled as before.

They don't exclude each other; the experiment asked what each is worth,
and whether parallel preparation (the only one that adds threads) earns
its place once the others are in.

## Experiment

`validate_layout()` at 1,024 and 10,000 cells (seconds, two runs each;
the scaling test's rack, bonded neighbours, so no contacts are reported):

| candidates | 4 threads, 1,024 | 4 threads, 10,000 | 1 thread, 10,000 |
| --- | ---: | ---: | ---: |
| none (0033) | 0.50 / 0.47 | 4.75 / 4.74 | 6.76 / 6.87 |
| 1 lazy copies | 0.42 / 0.45 | 4.11 / 4.14 | 6.40 / 6.58 |
| 2 parallel preparation | 0.38 / 0.31 | 3.59 / 3.32 | 6.22 / 6.22 |
| 3 names when reported | 0.37 / 0.41 | 3.91 / 4.08 | 5.13 / 5.48 |
| 4 normals without integrals | 0.39 / 0.40 | 3.86 / 4.10 | 5.04 / 4.79 |
| 5 skip settled samples | 0.35 / 0.35 | 4.01 / 3.79 | 3.71 / 3.81 |
| 2 and 3 | 0.35 / 0.32 | 3.30 / 3.60 | 5.13 / 5.47 |
| 4 and 5 | 0.25 / 0.25 | 2.53 / 2.42 | 2.05 / 2.18 |
| all but 2 | 0.06 / 0.07 | 0.72 / 0.64 | 1.04 / 1.02 |
| **all five** | **0.04 / 0.04** | **0.45 / 0.45** | **0.95 / 1.03** |

Each is small alone, because the others still dominate; together they
take 10,000 cells from 4.8 s to 0.45 s on four threads, and from 6.8 s to
1.0 s on one. Parallel preparation is worth 0.2 to 0.3 s once the rest
are in.

## Decision

All five. Nothing a report says changes, so no fixture, golden hash or
cache key changes either (reports feed none of them).

Two behaviours change only where something fails: a boundary whose deep
copy fails is now reported as undecided only by pairs that read it through
OCCT (before, by every pair it was in), and a rectangle on a box's side
that BRepMesh couldn't tessellate is no longer reported as undecided,
since it no longer needs to be tessellated. Neither happens to a valid
box or rectangle.

`LayoutOptions::sample_every_patch` turns candidate 5 off, for the test
that compares the two.

## Tests

- **Before committing**, a throwaway test ran every candidate on and off
  in one process and compared the reports as text, numbers printed to 17
  digits: the shared models (fixtures, bracket, curved, nested) on both
  paths, 6,000 random layouts from `test_box_path.cpp`'s generator (on
  three seeds, patches nudged by 0, ±5e-7, ±9.99e-7, ±1e-6, ±1.0005e-6,
  ±1.001e-6, ±2e-6 mm and more) and three 900-box grids jittered around
  the tolerance, each on one thread and four. All identical.
- `test_box_path.cpp`, "Unsampled patches on box sides change nothing":
  200 random layouts with those nudges and two jittered 900-box grids
  (several chunks of every parallel step), skipping and sampling, on one
  thread and four, must give the same report to the bit.
- `test_primitives.cpp`: `face_surface_plane()` and `face_plane()` give the
  same normal, bit for bit, on 100 random boxes (half turned about a
  random axis), 100 axis-aligned rectangles either way round, and say
  "not a plane" alike for a cylinder's side.
- Existing: the box path against OCCT (0032), the same report on several
  threads (0031). All 680 tests pass, and vs_validate and vs_eval with
  `VS_THREADS` 1 and 16.

## Results

`bench-rack --cells 10,100,1000,10000 --threads 4`
(`docs/benchmarks/bench-rack-2026-11-22-linux-checks.csv`):

| cells | preview ms, 0033 | preview ms, now | layout and checks ms, 0033 | now |
| ---: | ---: | ---: | ---: | ---: |
| 10 | 27 | 10 | 14 | 2 |
| 100 | 122 | 62 | 75 | 16 |
| 1,000 | 1,226 | 664 | 716 | 166 |
| 10,000 | 13,072 | 7,349 | 6,596 | 1,629 |

At 10,000 cells, layout and checks are 1.6 s (from 6.6 s), of which the
checks are about 0.5 s. Peak memory is unchanged (634 MB).

## Follow-ups

- The largest phases are now keys and cache (2.5 s), expand and check
  (1.6 s) and dependencies (1.5 s).
- Of what is left in the checks, each boundary's optimal bounding box
  (`BRepBndLib::AddOptimal`, also once per face for the box test) and
  `rectangle_of()`'s area integral are the likely costs; not measured.

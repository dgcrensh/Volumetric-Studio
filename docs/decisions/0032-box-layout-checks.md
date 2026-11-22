# 0032: Check boxes by arithmetic, within the tolerance of OCCT

Nov 22, 2026 · follows the document index (decision 0031)

## Context

After 0031, a preview of the 10,000-cell rack took 74 s, 92% of it in
layout and checks. The checks were linear but OCCT-bound: a fuzzy
`common` for every pair of neighbours that might overlap, face-by-face
`common_faces` and `min_distance` for contacts, and for every connection
surface a `min_distance` and a solid classifier over points spread on
the patch. Generated children are nearly always boxes, and for two boxes
all of that is min/max arithmetic. 0031 left this out because the
numbers would differ from OCCT's in the last bits; Dallas chose to accept
that, with a randomised test comparing the two.

## The rule

Results of the box path agree with OCCT's **within the linear tolerance**,
not bit for bit: the same codes, in the same order, with the same
subjects and face names; overlap depths and regions within the
tolerance, volumes and areas within the tolerance times the region's
surface. An edge or corner touch is reported at the middle of where the
two boxes meet (OCCT's nearest points can be anywhere along it).

No golden hash, cache key or saved fixture changes: layout reports feed
neither keys nor output hashes, and tests match codes.

## What changed

- **Which boundaries are boxes.** One solid, six planar faces, each facing
  out of a different side of its bounding box and spanning all of it
  (`box_sides()`, the test `fills_box()` already used for nesting).
  Decided once per volume. A keep-out cut, a notch or a curved face fails
  it, and any pair with such a boundary goes through OCCT as before.
- **Overlap** of two boxes: their intersection, when its thinnest side is
  deeper than the tolerance (the test that already let touching
  neighbours skip the boolean).
- **Contact**: boxes within the tolerance of each other touch face to face
  where one's side is within the tolerance of the other's opposite side
  and the shared rectangle is wider than the tolerance both ways and
  larger than the minimum feature size squared; otherwise along an edge
  or at a corner.
- **Connection rules**, for a patch that is a rectangle on an axis-aligned
  plane (planar, area equal to its box's) on a box boundary: its distance
  to the boundary is the boxes' gap, or from wholly inside, its nearest
  approach to a side; points on it are inside the box when more than the
  tolerance inside every side, as deep as their nearest side. The points
  are the same ones OCCT's path classifies. Bolted flanges (holes) and
  curved patches stay on OCCT.
- `LayoutOptions{box_fast_path}` (default on) lets tests run both paths.
- **`build_layout()`** looked up volumes (`index_of`) and the level's
  rebuilt features (`RebuildResult::find`) by linear search for every
  boundary and surface. With the checks fast, that was the next quadratic
  cost (21 s of the 10,000-cell build). Both are now found through maps
  made once per build; a miss still asks `find()`, so answers are what
  they were.

## The randomised guard

`test_box_path.cpp` builds 300 random layouts of two to four boxes (each
placed against another: overlapping, flush, sharing a side or apart, by
0, ±4e-7, ±1e-5 or ±0.01 mm), half with a bonded or clearance surface on
a side of the first box (nudged by those amounts, or 0.1 to 0.3 mm in,
and covering the side, part of it or more), and requires both paths to
agree as above. It also compares both paths on every shared model
(fixtures, bracket, curved, nested). Before committing, 12,500 layouts on
five seeds agreed.

What the guard found on the way:

- **A shared rectangle thinner than the tolerance** is an edge to OCCT.
  The box path first called it a face contact; it now requires both
  sides wider than the tolerance.
- **OCCT's fuzzy band.** Where sides are between half and three times the
  tolerance apart, OCCT decides either way: a 1.2e-6 mm overlap read as
  an edge touch, sides 1.2e-6 mm apart snapped together. The box path
  follows the stated rule (deeper than the tolerance overlaps). The guard
  leaves out layouts with a gap in that band (about 3% of them).
- **OCCT's distance from a patch wholly inside a solid** isn't always the
  nearest approach: 1.33 mm for a patch whose corner is 0.5 mm from a
  side. The box path reports 0.5. Both report `off_boundary` with the
  same subjects; the guard compares those messages without numbers.
  Where a patch inside is parallel to its nearest side, "is d mm from"
  and "reaches d mm inside" tie, and OCCT's rounding picks either; the
  guard reads them as one.

## Results

`vs_validate_tests "[.scaling]"` (seconds; cells narrow past 2,500 so the
rack stays in the working space), Linux cloud session, 4 vCPUs:

| cells | layout, 0031 | layout now | checks, 0031, 4 threads | checks now, 4 threads |
| ---: | ---: | ---: | ---: | ---: |
| 1,024 | 0.84 | 0.96 | 4.37 | 0.51 |
| 2,500 | 2.44 | 2.19 | 11.1 | 1.14 |
| 10,000 | 21.3 | 12.3 | | 5.14 |

`bench-rack --cells 10,100,1000,10000 --threads 4`
(`docs/benchmarks/bench-rack-2026-11-22-linux-boxes.csv`):

| cells | preview ms, 0031 | preview ms, now | again ms | peak MB |
| ---: | ---: | ---: | ---: | ---: |
| 10 | 60 | 22 | 2.4 | 19 |
| 100 | 558 | 196 | 19 | 26 |
| 1,000 | 5,849 | 1,894 | 231 | 80 |
| 10,000 | 74,356 | 23,015 | 4,548 | 638 |

At 10,000 cells, layout and checks are 17.1 s (from 68.4 s), of which
the layout build is most; keys and cache 2.8 s, expand and check 1.6 s,
dependencies 1.4 s.

## Follow-ups

- The layout build is now the largest phase: one OCCT box and one patch
  face per child, each passing `check_validity()`. Building boxes
  without the general checks, or in parallel, is the next step.
- Keys and cache hash every boundary's B-rep text (`shape_hash()`); a box
  could be keyed by its corners.

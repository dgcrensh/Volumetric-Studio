# 0026: H4, mesh geometry, file I/O and the CLI

Nov 21, 2026 · plan `docs/plans/H-hardening.md` (PR 28), phase H4

## Context

H4 fixes failure modes in the triangulator and the L2 curved seams, crash,
UB and parser bugs in the mesh and project file readers, and gaps in
`vstudio-cli`. As in H1 to H3, every item starts with a test that fails
on `main` (`21ed903`); an item that can't be reproduced is dropped and says
so here. Golden hashes, sample projects and every other suite's expected
values are unchanged: nothing here touches what a valid planar seam, a
field or a B-rep tessellation produces.

## Decisions

### The triangulator (`triangulate.cpp`)

**An exactly antisymmetric `orient()`, not Shewchuk's predicates.** The
plan offered a symmetric tolerance or adaptive `orient2d` / `incircle`.
Fuzzing outlines and seams (lattices, slivers, mixed scales, up to ±1,000
mm off the origin) found a seam-like strip far from the origin (13,690
points near (727, -793), 0.3 × 12.9 mm, divided and filled as a seam is)
whose point insertion failed on `main` for five spacings in eighty-one.
The cause was `orient(a, b, c)` and `orient(b, a, c)` disagreeing in the
last bit, and `orient_eps()` depending on which corner it was measured
from. `orient()` now sorts its three points and applies the permutation's
sign, so every test that sees three points agrees on their side;
`orient_eps()` uses the two longest sides. With that, point location walks
across an edge on its raw sign (the walk can no longer stop in a flat
neighbour), and a point within rounding of two edges of a sliver lies on
the one it projects inside. Exact predicates were not needed for anything
we could reproduce, and would have changed every seam's triangles.

**The super triangle has no size floor.** It was at least 1e-9 mm across,
so a polygon at 1e-100 sat "on" its edges. It is now sized from the
points' extent alone.

**A flip cap and constrained checks.** `legalise()` stops (and the
triangulation fails) after `2 × triangles + 64` flips, more than any
Delaunay insertion needs; `recover()` fails on a constrained edge in its
way instead of flipping an earlier segment away; crossing tests compare
signs rather than multiplying two areas, which underflowed to zero at
1e-100 (tested at 1e-100, 1e-6, 1 and 1e60 mm). No input reached the cap
in fuzzing; it is there so a cycle fails instead of hanging.

**A bounding-volume tree instead of the grid in `check_segments()`.** The
plan suggested an x-sorted sweep. The grid's cells were twice the mean
segment length, so a long diagonal over finely divided legs covered tens of
millions of cells: the test's right triangle (25,000 pieces per leg) peaked
at about 5 GB on `main`. A sweep degrades the same way when many segments
share an x range (a finely divided vertical side), so segments go into a
tree of boxes (median split, leaves of up to four) and each segment
queries it: 21 MB for the same triangle. The test checks the peak on Linux.

### Curved seams, tessellation and weld

**Band spacing is capped at a sixteenth of the circumference.** A coarser
spacing left a tall band with no interior points, and its triangles reached
from one side of the cut to the other, more than half a turn round the
axis, which `triangle_uv()` (and the weld after it) can't tell from the
short way round. `Seam::spacing` is the spacing actually used. Planar and
partial curved seams are unchanged.

**Bands are imprinted as two half turns.** The plan named wraparound in
`divide_on_seam()`'s box test. The test that reproduces it (a shaft and
sleeve whose own seam lines lie on the band's cut, five angles) found the
cause one step earlier: a whole-turn imprint face's own seam edge lay on
the solids' seam lines, and OCCT's splitter returned them unsplit; with the
imprint's seam elsewhere, the splitter left a slit edge inside the
remaining face. Two half-turn faces split cleanly in both cases. With that,
`divide_on_seam()` never meets an edge across the cut with seam vertices
inside it, so its wraparound change was dropped (it made no test pass).

**Imprint failures are reported.** `make_cylinder_face()` failing (a band
thinner than the minimum feature size) was dropped silently and showed as
"no face of the volume covers the patch"; its `kernel.*` diagnostic now
reaches the volume.

**Weld conformity counts hole fills.** A volume's hole fill triangles are
part of its seam conformity: a hole filled in part is
`mesh.weld.nonconforming` ("1 hole fill partial") rather than an open edge
later. The message also counts repeated seam triangles ("0 of N missing, 1
repeated" used to read only "0 of N missing").

### Mesh invariants and files

- `-0.0f` and `+0.0f` are one position: the mesh readers' `Builder`
  canonicalises them, and `attach_seam_vertices()` treats them as equal, so
  a baked or imported mesh whose seam vertices carry `-0` welds.
- `Mesh::check()` refuses non-finite positions, and `write_stl()` /
  `write_3mf()` run it first; the ASCII STL, OBJ and 3MF readers refuse a
  coordinate that overflows a float (after a 3MF item's transform too).
- `Mesh::append(*this)` is safe (it was undefined behaviour, which aborts
  under `_GLIBCXX_DEBUG`).
- `enclosed_volume()` and `eval::measure_mesh()` sum relative to the first
  vertex: a cube near the working space's corner lost digits before.
- `read_obj()`: `k < -n` (no `LLONG_MIN` negation), inline `#` comments.
- `read_3mf()`: end tags carry nothing (a `<vertex ...></vertex>` counted
  twice), namespace prefixes are ignored (`m:vertex`), numeric character
  references are decoded, and the current object is a pointer.
- `.vsproj`: reading a folder where a blob should be (`blobs/.`) is
  `model.project.blob_missing` instead of `bad_alloc`; every directory
  iteration uses the `error_code` overloads; each write has its own
  temporary name (two saves of one zip used to rename each other's file
  away) and checks `close()`; on Windows, where replacing a file another
  rename is replacing fails for a moment, the rename is retried briefly. A manifest `version` above the format's, or
  not a whole number, is refused (2³² + 1 was read as 1).
- Cache entries: the same temporary names; a severity or subject kind out
  of range is a miss.

### `vstudio-cli`

- `rebuild --3mf <file> [--per-volume]` writes the engine's welded mesh (or
  one object per volume), so procedural projects export 3MF.
- `rebuild --cache` on a zip project reads its `cache/` entries into the
  engine and saves the zip back with the new caches (and the project as it
  was read, never `--set`'s changes).
- `--gpu` opens the GPU only for `rebuild`, `inspect`, `bake` and
  `bench-gyroid`; other commands ignore it.
- `bench-gyroid --threads` takes a whole number from 0 to 256 (`1.5` and
  `1e9` passed before), and a 3MF it can't write fails the command.
- One `to_path()` and `write_bytes()` (`src/files.hpp`) for every command.
- `selftest` reports whether vs_gpu is available and matches the CPU
  ("not available" off Windows); it is information, not a check.

## Dropped (not reproduced)

- **Weld facing by cyclic parity.** Every wrongly facing seam triangle we
  could make came from the band seams above; with the spacing capped,
  `triangle_uv()`'s cross product judged them all correctly.
- **The shorter arc pair on closed edges** (`divide_on_seam()`): the
  outline piece's midpoint already picks the right pair; no edge we built
  mismatched.
- **An interior sample in `patch_piece()`.** A shaft under a patch of 350°
  or more (remainders of 0.6° to 8.6°, radii 2 and 10) keeps its remainder
  out of the patch: BRepMesh always divides the remainder's arcs, and the
  middle point lies outside. The test stays as a guard.
- **Orphaned Steiner points from collapsed wires** and **de-duplicating
  BRepMesh's interior points against the remainder lattice**: no face we
  built had a wire of fewer than three points, and BRepMesh puts no
  interior points in a cylindrical face, so the lattice has nothing to
  collide with.

## Tests

`libs/vs_mesh/tests/test_hardening.cpp`, `libs/vs_model/tests/test_hardening.cpp`,
`libs/vs_eval/tests/test_hardening.cpp`, the `[hardening]` cases at the end
of `libs/vs_mesh/tests/test_curved.cpp`, and the H4 block of
`apps/vstudio-cli/CMakeLists.txt` (procedural 3MF, per-volume 3MF, a zip
project's cache saved and read back, `--gpu` ignored, `selftest`,
`bench-gyroid`'s arguments and write errors). Each fails on `main`, except
the 350° guard.

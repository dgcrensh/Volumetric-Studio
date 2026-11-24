# 0042: Voronoi walls seeded from a surface

Nov 24, 2026 · plan: `docs/plans/V1-voronoi-walls.md`

## Questions

1. Build the Voronoi field ourselves, or take a library?
2. What exactly does the field measure, and how does it stay fast and
   exact on the CPU and the GPU?
3. How do seeds on a surface get into the volume?
4. What is left out of the first step?

## Decisions

**Our own field op, no library.** The work that matters here is the
GPU mirror and bit-exact determinism (decisions 0018, 0019), and no
library helps with either. Voro++ (BSD) builds explicit cells, useful
for edge struts later; CGAL's 3D triangulation is GPL; nanoflann would
only speed the CPU half; GPU jump flooding is approximate and can't
match the CPU. The search is a uniform grid, about fifty lines a side.

**The field is the exact distance to the cell walls.** `FieldOp::Voronoi`
(`voronoi_field()`) at p, with a the nearest seed, is

    min over b != a of (|p - b|^2 - |p - a|^2) / (2 |b - a|)  -  t / 2

the distance from p to the nearest bisector plane of a and another seed.
A cell is convex, so for a point inside it that is the exact distance to
its boundary; the walls are the set within t/2 of it. Away from the
walls it is still a distance (not a bound), which suits the clip and
the mesher.

**A uniform grid, searched shell by shell.** The seeds' box is cut into
cells `longest side / m` wide, m the largest whole number up to 128 with
no more cells than seeds (whole-number arithmetic and one division, no
`cbrt`, so the numbers are the same everywhere). Seeds are stored sorted
by cell, after a table of each cell's first seed, all in the node's
numbers (`field_layout.hpp`), so the digest, node cache and GPU buffer
need nothing new. A query visits shells of cells r = 0, 1, 2 ... round
the point's cell (clamped to the grid) with a lower bound on the
distance to any seed in the shell: each axis's gap to the grid, plus the
smallest extra the r-th cell along one axis adds. The nearest-seed pass
stops when the bound passes the best; the wall pass stops when
(bound − |p − a|) / 2 reaches the best wall, because
(|p − b|² − |p − a|²) / (2|b − a|) ≥ (|p − b| − |p − a|) / 2.
Points well outside a flat layer of seeds stay cheap: an axis the grid
has one cell along adds nothing per shell, so the search reaches the
layer at once.

**Exact, and the same on both sides.** The value is a minimum, so the
order of the search doesn't change it, and ties for the nearest seed
(lowest index) give a wall of 0 whichever wins. It equals a brute-force
minimum over every pair bit for bit (`test_voronoi.cpp`, 2 to 2,000
seeds, flat layers and clusters). The only root is correctly rounded on
both sides (`sqrt_rn()` in GLSL). The GLSL (`voronoi()` in `glsl.cpp`)
is the C++ step for step, with no local arrays of doubles (decision
0019); `self_check()` compares three Voronoi fields. `Voronoi` is
appended to `FieldOp`, so no other digest, golden value or cache key
moves and `kEngineVersion` stays 2.

**Seeds go in along their own normal.** `frames.offset` moves each
frame along its z axis, `layers` copies at `distance + k · step`.
`into_boundary` (the default) picks per frame the side whose first
point is deeper inside the boundary's signed distance, so it works on
curved surfaces, whose normals point away from their axis whichever
side the volume is on; `margin` drops points less than that deep. It
reads the boundary, so it isn't node-cached (like `scatter.volume`).

**The template.** `voronoi_walls` is scatter on a connection surface →
`frames.offset` (margin = thickness) → `field.voronoi`, intersected with
the boundary and joined to a `wall` skin like the other templates.

**Left for later.** Thickness graded by a field; seeds from the
boundary's own skin or one named face of it (connection surfaces only,
for now); edge struts; a dedicated benchmark.

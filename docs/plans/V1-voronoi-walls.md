# V1 — Voronoi walls from a surface (plan)

Nov 24, 2026 · decision record: `docs/decisions/0042-voronoi-walls.md`

Status: first PR done (CPU and GLSL; the GPU check runs on the desktop).

## Goal

Fill a volume with Voronoi cell walls whose seeds come from a surface
and are moved inward, so the cells follow the surface's shape:

    scatter.surface → frames.offset → field.voronoi → ∩ input.boundary → contents

## First PR (done)

- `frames.offset` (`nodes_frames.cpp`): frames moved along their own z,
  `layers` copies at `distance + k · step`, `into_boundary` or `along_z`,
  `margin` dropping shallow points.
- `field.voronoi` (`nodes_infill.cpp`) and `FieldOp::Voronoi`
  (`voronoi_field()`): walls of a constant thickness; refuses fewer than
  two seeds, coincident seeds and walls under the minimum feature size.
- The GLSL mirror (`glsl.cpp`) and three self-check fields (`sampler.cpp`).
- The `voronoi_walls` template and its saved copy.

Acceptance: the field equals a brute-force minimum bit for bit; walls on
the bisector planes; existing goldens unchanged; offset on flat and
curved surfaces, layers and margin; node checks; the template contained,
welded and identical on 1 and 3 threads; a cancelled evaluation keeps
nothing; `vstudio-cli templates` lists it; `vs_gpu_tests` on a GPU.

## Later

1. Seeds from the boundary's skin (`scatter.boundary` on the boundary
   mesh), then from one named face (`PropertyKind::BoundaryFace`,
   tessellated by `gather_inputs()` when named).
2. Thickness graded by a field, as lattices and TPMS are.
3. Edge struts (where three cells meet), possibly through Voro++.
4. A `bench-voronoi` command.

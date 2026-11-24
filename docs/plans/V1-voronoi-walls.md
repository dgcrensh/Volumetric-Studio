# V1 — Voronoi walls from a surface (plan)

Nov 24, 2026 · decision record: `docs/decisions/0042-voronoi-walls.md`

Status: first PR done (CPU and GLSL; the GPU check runs on the desktop);
second PR (seeds from the boundary) done.

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

## Second PR: seeds from the boundary (done)

- `scatter.boundary` (`nodes_frames.cpp`): points on the boundary's skin
  by area, or on one named face (`face`, a `PropertyKind::BoundaryFace`:
  `-x`..`+z` on a box, `<feature id>:<name>` on a feature boundary;
  empty is the whole skin), each frame's z out of the volume. Shares
  its dart throwing with `scatter.surface`, whose frames are unchanged.
- `gather_inputs(..., GatherOptions)` tessellates only the named faces a
  graph reads (`boundary_faces_read()`), found with
  `feature::on_named_face()`, into `GraphInputs::boundary_faces`. The
  generator, inspection and the app's node previews pass them.
- `proc.reference.boundary_face`: `check_volume_graph()` refuses a name
  the boundary can't have; `gather_inputs()` one it doesn't have once
  built (or one a keep-out cut away); evaluation one it wasn't given.
- The app's node form lists the boundary's faces from the last
  evaluation (`GraphContext::boundary_faces`, labelled as the feature
  dialogs label them), with "(whole boundary)" first.
- The `voronoi_skin` template (`TemplateOptions::boundary_face`).

Acceptance: points on the skin, spaced, seeded, facing out on every
side; one face of a box, and the cylinders of the curved fixture facing
out of the shaft and out of the sleeve's bore; missing faces refused at
each stage; `voronoi_skin` seeded on a named face through the engine,
contained, welded and identical on 1 and 3 threads.

## Later

1. Thickness graded by a field, as lattices and TPMS are.
2. Edge struts (where three cells meet), possibly through Voro++.
3. A `bench-voronoi` command.

# L2 — Curved connection surfaces (plan)

Nov 20, 2026 · follows L1b · decision record: `docs/decisions/0020-curved-connection-surfaces.md`

**Status: built.** As below, with three differences found while building it
(decision 0020 has the details): the derived spacing is `sqrt(8 r δ) / 1.5`,
because the triangulation's edges run up to about 1.5 spacings round the
axis; what an imprinted patch leaves of a cylinder's face gets a lattice of
points at the seam's spacing, because BRepMesh puts none inside a cylinder;
and containment gives triangles touching a curved seam a centroid slack of
twice the seam deflection rather than skipping only the seam's own
triangles, because the strip triangles next to them sag too.

## Goal

Let a connection surface be curved, so the joints mechanical parts
actually have can be declared: a shaft bonded in a bore, a sleeve round a
tube, a curved panel on a curved shell, a clearance fit between a pin and
its hole. Every rule that holds for planar surfaces holds for curved ones:
the surface is an authored object, both volumes take it as a fixed input,
every mesher follows its seam exactly, and the combined mesh welds
structurally, watertight and manifold, with no booleans.

It also answers the spec's open question: **should a curved connection
surface's seam definition be authored, or derived automatically?**

## The answer to the open question: derived, with the spacing authored

A seam stays a pure function of the patch and a spacing, as decision 0007
made it for planes. On a curved patch the derived spacing is the smaller
of the planar rule (a sixteenth of the diagonal) and the spacing whose
chords stay within the seam deflection of the surface
(`kSeamDeflection`, 0.01 mm, the same deflection the boundary SDF and
B-rep tessellation already use): on a cylinder of radius r that is
`sqrt(8 r δ)`. `SeamDefinition::spacing` still overrides it, so the one
thing a user may want to control (coarser for speed, finer for looks) is
authored; the vertices themselves are never authored. Authoring vertex
positions would add a second object to keep in step with the patch every
time a layout dimension changes, for no gain the spacing doesn't give.

## Scope

**Cylindrical patches only.** Cylinders cover the joints above, and a
cylinder unrolls exactly (arc length round, distance along the axis), so
the seam is the planar machinery in unrolled coordinates: outline
division, triangular lattice, constrained Delaunay, all unchanged. Cones
are also developable and would follow the same pattern later; spheres and
freeform surfaces need a parameter-space metric and are not in L2.

- **Model** (`vs_model`). A third patch kind, `CylinderPatch`: a layout
  axis (`AxisRef`), a radius, a start angle and a sweep round the axis,
  and a start and length along it. A sweep of a full turn is a **band**
  (a ring round the axis, the shaft-in-bore case); anything less is a
  curved rectangle. Angle 0 is a fixed direction perpendicular to the
  axis, derived from the axis alone. `ConnectionBuild` gains the
  cylinder; its `frame` is origin on the axis, normal along it, x axis at
  angle 0. JSON (`"cylinder": {...}`), `check()`, dependencies,
  `vstudio-cli info`. Bolted flanges stay planar: a bolted flange on a
  curved patch is refused by `check()` (`model.connection.curved_flange`),
  because a radial bolt hole's outline on a cylinder is not a circle in
  any frame, and nothing the MVP makes needs it. Bonded and clearance
  surfaces may be curved.
- **Contents' inputs.** Inside a parametric volume a curved surface's
  `patch` face is a cylinder (`NamedFace::cylinder`), as boundary faces of
  bores already are; the surface's own reference is its axis.
- **Seams** (`vs_mesh`). `SeamOutline` and `Seam` gain a surface: plane
  (as now) or cylinder (radius). On a cylinder, `uv` is unrolled
  (s = r·θ, t along the axis), positions come from the cylinder, and
  triangles counter-clockwise in (s, t) face away from the axis. A band
  is triangulated as the unrolled rectangle whose two ends are divided
  identically and then identified, so the seam is a closed ring with two
  boundary loops and no hole fills. Derived spacing as above.
- **B-rep tessellation.** Planar seams keep today's path untouched. For a
  cylindrical seam the kernel first **imprints** the patch on the solid
  (the face lying on the patch's cylinder is split along the patch's
  outline, `kernel::tessellate()` taking faces to imprint). The piece
  that is the patch is replaced by the seam's triangles, the edges along
  the outline take the seam's division (step 2, as now), and the rest of
  the face is re-triangulated in its own parameter plane (step 4, as
  now). That avoids triangulating a cylinder face in a frame its own
  seam line may cross.
- **Procedural meshing** (`vs_proc`). The stitch generalises from "a
  plane" to the seam's surface: distance along the surface's normal,
  facing against it, (s, t) for inside tests and areas. A band removes an
  annulus of grid triangles, so the stitch joins each rim to its nearest
  seam loop; a partial patch is one disc, as now. The planar arithmetic
  is unchanged, so planar meshes stay bit for bit.
- **Containment.** A seam's triangles are chords of a curved surface, so
  on the concave side (the bore's volume) their centroids sit up to the
  seam deflection outside the exact boundary. Both volumes share those
  triangles by definition, so containment of mesh contents skips the
  centroids of triangles whose three corners are seam vertices (their
  corners are still checked; they lie on the surface). Planar results
  are unchanged, since those centroids are on the plane.
- **Validation** already works on kernel geometry (the patch's distance
  to each boundary, clearance gaps, contact between faces), so it gains
  tests rather than code: a cylinder patch off the boundary, a clearance
  fit, undeclared contact between a shaft and a bore.
- **App.** The connection-surface dialog offers "Cylinder" beside sketch
  profile and datum rectangle; the outline names it; the viewport draws
  the patch from its face as it does now.

## Tests (acceptance)

1. A cylinder patch builds (the face's area is the unrolled rectangle's),
   round-trips through JSON exactly and is in the dependency graph; a
   curved bolted flange is refused.
2. Seams on cylinders: every vertex on the cylinder, every chord within
   the seam deflection with the derived spacing, area matching the
   surface's, triangles facing away from the axis, a band closed except
   for its two loops; random radii, sweeps, lengths and frames.
3. B-rep pairs weld into one watertight, manifold mesh of the right
   volume: a shaft bonded in a sleeve (band), a curved panel on a curved
   shell (partial patch), patches smaller than the faces they lie on,
   in random rigid poses.
4. A procedural volume bonded to a parametric one across a band and
   across a partial patch welds through the engine, and passes
   containment.
5. Validation: off-boundary, clearance-gap and contact fixtures for
   curved surfaces give exactly the expected diagnostics.
6. Every existing test, golden hash and sample project is unchanged.

## Out of scope

Cones, spheres and freeform patches; curved bolted flanges; sketches
wrapped onto cylinders as patch outlines (a curved rectangle or band
only); seams spanning two patches that share an edge.

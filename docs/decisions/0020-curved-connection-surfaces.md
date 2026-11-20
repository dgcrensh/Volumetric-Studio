# 0020: Curved connection surfaces

Nov 20, 2026 · L2 · plan: `docs/plans/L2-curved-connection-surfaces.md`

## Questions

1. The spec's open question: should a curved connection surface's seam
   definition be authored, or derived automatically?
2. Which curved surfaces, authored how, and which connection types?
3. How is a seam laid out on a curved patch, and how does a band (a ring
   round an axis, with no ends round it) close?
4. M8's tessellator re-triangulates planar faces in the seam's frame. A
   cylinder face's own seam line can cross the patch anywhere. How does a
   B-rep volume conform to a curved seam?
5. M11's stitch removes grid triangles "on the patch's plane" and joins one
   rim to one outline. How does it work on a cylinder, and on a band, whose
   patch leaves two rims?
6. A seam's triangles are chords of the curve. On the concave side (the
   bore's volume) they lie outside the exact boundary. What does
   containment make of that?

## Decisions

**Derived, with the spacing authored.** A seam stays a pure function of
the patch and a spacing (decision 0007). On a curved patch the derived
spacing is the smaller of the planar rule (a sixteenth of the diagonal)
and `sqrt(8 r δ) / 1.5`, with δ = `kSeamDeflection` (0.01 mm, the boundary
tessellation's deflection): a chord running h round a radius-r axis
strays h²/8r from it, and the triangulation's edges run up to about 1.5
spacings round the axis (by the outline, where the lattice keeps half a
spacing away). 120 random seams in the suite measured at most 1.44.
`SeamDefinition::spacing` still overrides it. Vertex positions are never
authored: they would be a second object to keep in step with the patch
whenever a layout dimension moves, for nothing the spacing doesn't give.

**Cylinders, as a third patch kind.** `CylinderPatch`: a layout axis, a
radius, a start angle and a sweep round it, a start and a length along it.
A sweep of a full turn is a band (shaft in a bore, sleeve round a tube);
less is a curved rectangle. Angle 0 is `cylinder_frame()`'s x axis, which
depends on the axis alone (the principal axis least aligned with it, made
perpendicular), so the patch doesn't move when anything else does. The
patch is a `kernel::make_cylinder_face()`. Inside a joined parametric
volume the surface is its axis and a cylindrical `patch` face, as bore
faces of boundaries already are. Bonded and clearance surfaces may be
curved; a bolted flange may not (`model.connection.curved_flange`, from
`check()` and `build()`): a radial bolt hole meets a cylinder in a saddle,
not a circle, so the hole loops of decision 0007 don't carry over, and no
joint the MVP makes needs it. Cylinders unroll exactly, so cones would
follow the same pattern; spheres and freeform patches need a
parameter-space metric and are left for later.

**Seams in unrolled coordinates.** On a cylinder, `uv` is (r·θ, distance
along the axis), and everything planar carries over: outline division
(lines round the axis at least 16 pieces a turn, as arcs), the triangular
lattice, constrained Delaunay. Triangles counter-clockwise in uv face away
from the axis. A band's rectangle is triangulated whole with its two ends
divided identically, then the right end's vertices are identified with
the left's, so the seam is a closed ring with two loops (`Seam::band`) and
no hole fills; `to_uv()` gives every point the u in [x0, x0 + 2πr), and
`triangle_uv()` unwraps a triangle across the cut. Seam keys hash a
different tag, the radius and the band flag, so curved and planar seams
never share ids, and planar seams are byte for byte what they were.

**B-rep volumes: imprint the patch, then replace it.** Before meshing, the
kernel splits the solid's faces along each curved patch's outline
(`kernel::tessellate(..., imprints)`, `BRepAlgoAPI_Splitter` with the patch
faces as tools, fuzzy at the tolerance), so the patch becomes faces of its
own. A cylindrical face on the seam's cylinder lying wholly inside the
patch is a piece of it: its triangles are dropped, and once the pieces'
exact area is the patch's, the seam's triangles go in their place, facing
out of the material. Edges along the outline take the seam's division
(M8's step 2, generalised through `Seam::offset()`, `to_uv()` and
`to_model()`), and what the patch left of the face is re-triangulated in
its own parameter plane (step 4) with a lattice at the seam's spacing:
BRepMesh puts no points inside a cylinder, and without them the finely
divided boundary was joined by chords straight across the face (a shaft
lost 12 % of its volume that way). Working in the face's own parameters
sidesteps OCCT's seam line, which may cross the patch anywhere. A face
covering only part of the patch, or volumes on both sides, is
`mesh.tessellate.seam_uncovered`. One existing rule was loosened: an
edge end bound to a seam vertex now counts wherever that vertex lies
(a snapped end could sit outside the edge's box and leave a BRepMesh point
on the outline); planar results are unchanged.

**Procedural volumes: the same stitch, through the seam's surface.**
Heights are along the surface's normal at each point (away from the axis,
signed by which side the volume is on), inside and near tests use uv (a
band's: between its two ends), and the stitch handles one rim per loop of
the seam's face: a band's removal is an annulus, each rim paired with the
seam loop at its height, checked to wind the same way and to track it in
space within three cells. Planar seams go through exactly the arithmetic
they did, so their meshes and hashes are unchanged.

**Containment allows a curved seam's chords.** Both volumes share a
seam's triangles by definition, so the chord sag is the agreed
discretisation of the shared surface, not contents crossing it. The
engine gives every triangle touching a curved seam's vertex a centroid
slack of `2 kSeamDeflection` (the seam's chords, plus the boundary
tessellation the mesher's containment loop works against) through the new
`MeshView::centroid_slack`; vertices are still checked strictly, and
volumes joined by no curved surface get no slack at all. B-rep contents
are still checked exactly on their solids.

## Consequences

- `vs_kernel`: `make_cylinder_face()`; `tessellate()` takes imprints;
  `TessFace::cylinder`; `face_point()`.
- `vs_model`: `CylinderPatch`, `CylinderBuild` on `ConnectionBuild`,
  `cylinder_frame()`; codes `model.connection.axis_failed` and
  `.curved_flange`. JSON `"cylinder": {axis, radius, start_angle, sweep,
  start, length}`; documents without one are unchanged.
- `vs_mesh`: `SeamOutline::radius` / `band`, `Seam` surface queries,
  `kSeamDeflection`; the weld judges facing in unwrapped uv.
- `vs_validate`: nothing new in the checks (they work on kernel geometry);
  `MeshView::centroid_slack`.
- Tests: seams on 120 random cylinders and frames (on the surface, chords
  within δ, exact unrolled area, facing, ring or disc topology,
  determinism); shafts in sleeves, half shells, quarter pads and patches
  smaller than their faces weld into one closed shell of the right
  volume, 24 of them in random poses; documents weld through the seam
  library; validation fixtures (off-boundary, clearance fit and a too-small
  gap, undeclared contact); procedural shafts and sleeves weld to
  parametric ones through the engine on one or four threads with the same
  hash.
- App: the connection-surface dialog's third patch kind, the outline
  names it, picking matches a curved patch. `vstudio-cli seams` says
  "cylinder" and checks a band is a ring.

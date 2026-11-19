# 0011: Field to mesh against seams

Nov 19, 2026 · M11

## Questions

1. Plain surface nets (one vertex per grid cube) aren't always manifold:
   a cube whose surface touches one face in two places, as a thin wall or
   a tunnel does, gives an edge four triangles. The weld rejects that.
   How do we keep surface nets and still get a closed 2-manifold for any
   field?
2. "Intersect the field with the boundary SDF so output stays inside":
   is clipping enough for containment, which allows the linear tolerance
   (1e-6 mm) plus FP32 rounding and nothing more?
3. How does a grid surface take "the shared seam triangulation from M7",
   and what if the contents don't cover the patch?
4. What does the explicit Field → Mesh node do, given that the volume's
   own contents are meshed by the generator, and how is its cost visible?
5. Where do the mesh, its containment check and its weld live, given the
   module order (`vs_mesh` and `vs_eval` below `vs_proc`)?

## Decisions

**Surface nets on the grid's tetrahedra, then merged per cube.** Each
cube is split into six tetrahedra round its 0–7 diagonal, the same way
everywhere, so neighbouring cubes' tetrahedra meet face to face. A
tetrahedron's surface is always a single disc, so one vertex per crossed
tetrahedron (the mean of its edge crossings) and one polygon per crossed
edge (the tetrahedra round it, in order) is a closed 2-manifold for any
samples. That is about six times the triangles of cube surface nets, so
each cube's vertices are then merged into one by edge collapses that pass
the link condition (the two ends' common neighbours are exactly the two
opposite corners) and leave every shell at least four vertices. A cube
whose surface is one simple disc ends with one vertex, giving about two
triangles per crossed cell as cube surface nets do (a 40 × 30 × 20 mm
shelled box at 1 mm: 19,000 triangles instead of 105,000); the tangled
cubes keep the vertices they need. Faces of a tetrahedron are never
ambiguous, so no face-resolution rule is needed. Tests run 3,000 random
grids (exact zeros included) without a single open or non-manifold edge.
Dual contouring can still replace this behind the same functions later,
as the plan says.

**Clip with an inset, then move vertices until containment holds.**
Clipping alone isn't enough. Surface-nets vertices are means of crossing
points, and a mean round a re-entrant (concave) edge lies outside; so
does a triangle spanning that edge, even when its corners are inside. And
the boundary SDF is the boundary's tessellation, whose facets lie up to
the deflection (0.01 mm) outside a concave curved face. So the field is
clipped at the boundary's tessellation inset by its deflection
(`kContentsInset` = `kInputDeflection`), and after meshing a short loop
moves vertices inward against the boundary's gradient until every vertex
and every triangle's centroid, judged from the FP32 positions, is inside
by the inset (on the boundary for triangles touching a seam). That is
exactly what `validate::check_containment()` samples, so the generator
never produces a mesh the engine then rejects. Planar faces end 0.01 mm
inside the exact boundary, which is far below anything printable.
Containment holds for 24 random fields in random boundaries in the suite
(boxes, cylinders on random axes, spheres, Ls, boxes with round holes)
and for 300 in a longer run made while writing it.

**Seams replace the patch's grid triangles and are stitched to its
rim.** For each seam joining the volume, the grid triangles on the patch
(within a cell of its plane, facing out, inside the outline or within a
cell of it) are removed; the seam's triangles and every hole fill go in
facing out; and the rim the removal leaves is joined to the seam's outer
loop by a strip that walks both loops together by arc length, from the
rim vertex nearest the outline's first. Near the outline the removal
also takes the grid's rounding of the boundary's edge, two cells deep,
whatever way it faces: where a patch runs to an edge of the boundary, the
rim then sits on the side wall below the outline and the strip continues
the wall, instead of cutting a groove along it. Where the rim touches
itself at a vertex, the triangles round that vertex join the removal.
Directions come from topology, not geometry: the rim runs the removed
triangles' way and the outer loop the seam face's way, and each strip
triangle uses those edges reversed, so the result is closed and
consistently oriented whatever the strip looks like.

A seam is stitched only when the contents cover the patch: the removal
must be one disc whose rim stays within three cells of the outline all
the way round, running the same way. Otherwise the volume keeps its grid
surface there (still closed and contained) and gets the warning
`proc.mesh.seam_uncovered`, naming the surface first; the weld then fails
there with `mesh.weld.nonconforming`. It is a warning, not a failure,
because the volume's contents are still valid on their own; only the weld
needs the seam. Filling a gap in the patch ourselves would change the
user's geometry.

**The volume's mesh is automatic; the node is for meshes inside a
graph.** The output node's `cell_size` is the grid for both the samples
and the mesh. `convert.field_to_mesh` meshes a field over the boundary's
box (no clip, no seams) and outputs `samples`, the number of grid points
it evaluated, alongside the mesh and its triangle count, so the cost of a
finer `cell_size` shows in the graph; past `kMaxGridSamples` it is
`proc.node.infeasible` naming `cell_size`. `convert.mesh_to_field` gives
the signed distance to a closed, manifold mesh (unsigned otherwise, with
`closed` saying which) through the same BVH as the boundary.

**The mesh is generated contents; the engine checks it and the weld
takes it as it is.** The generator puts the mesh in
`GeneratedContents::mesh`, so the engine's existing containment check for
mesh contents runs on it (on the calling thread), and keeps the field and
samples in `FieldContents` as before (with the surfaces it stitched). The
output hash covers both. `mesh::combine()` takes a map of generated
meshes from the caller; the engine passes every non-static volume's mesh,
and those go into the weld unchanged. The generator makes its seams with
`build_seams(document, layout, settings, volume)`, which builds only the
surfaces joining that volume and gives exactly the library's seams (same
inputs, same ids). Meshes stay out of the disk cache, as field contents
did.

## Consequences

- A procedural volume next to a parametric one welds into one watertight,
  manifold mesh across a bonded or bolted-flange surface (acceptance 1);
  so do rows of procedural volumes.
- Every M10 result still holds (samples are unchanged), but procedural
  contents now regenerate a mesh and run the containment check, which
  dominates their time (about 0.4 s for the 1 mm shelled box).
- Box edges come out rounded by about a cell and a half (surface nets
  keep no sharp features); patches' own edges are exact, because the seam
  replaces the grid there.
- Published outputs of procedural volumes still have no value: measuring
  is M12.
- `vstudio-cli rebuild` prints each procedural mesh and `--stl <file>`
  writes the welded mesh when it is clean.

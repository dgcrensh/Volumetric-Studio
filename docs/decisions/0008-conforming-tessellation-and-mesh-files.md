# 0008: Conforming B-rep tessellation, the combined mesh and mesh files

Nov 19, 2026 · M8

## Questions

1. The spec's open question: can the tessellator respect fixed seam edges
   on B-rep faces? The plan's sketch was "faces on connection surfaces use
   the M7 seam triangulation; other faces use OCCT's BRepMesh with their
   boundary edges pre-discretised to match seam vertices". How much of
   that can BRepMesh do, and where does the code live, given that OCCT
   stays inside `vs_kernel` and the seams live in `vs_mesh` above it?
2. What happens where a volume's face lies on a seam but isn't exactly
   the patch: a larger face round it (the bracket's base), a bolt hole in
   the seam that the solid doesn't have, or a face that covers only part
   of the patch?
3. What if a B-rep vertex sits on the seam's outline between two seam
   vertices (a circle whose start point isn't the seam's)?
4. Which geometry does the combined mesh use for each kind of volume?
5. 3MF needs a zip writer and an XML reader. Which?

## Decisions

**Yes, with BRepMesh as a source of points rather than the final word.**
Steering BRepMesh's own edge discretisation means subclassing its
`IMeshTools` model algorithms, which are internal and change between OCCT
versions. Instead `kernel::tessellate()` runs BRepMesh normally and hands
back plain data: every vertex, every edge with BRepMesh's division of it
(parameters and points), every face's wires as ordered edge uses (the
seam of a closed surface twice, degenerate edges included) and its
triangulation with, for each point, whether it lies inside the face, on a
vertex or at the k-th point of an edge. Two small queries
(`edge_parameter()`, `edge_uv()`) let the caller place points of its own.
The kernel knows nothing about seams; `vs_mesh::tessellate()` does the
rest:

1. B-rep vertices on a seam's outline become its seam vertices.
2. Edges lying along the outline are divided at the seam's outline
   vertices (found with `edge_parameter()`); BRepMesh's points stay on
   any part of the edge off the outline, unless they crowd a seam vertex.
3. Planar faces on the seam's plane are re-triangulated in the seam's
   frame with our constrained Delaunay triangulator, the patch outline
   drawn across the face as extra constrained segments (`triangulate()`
   gained that overload). Triangles inside the patch are thrown away and
   the seam's own go in their place, so the face is exactly the seam
   inside the patch and the face's own triangulation outside it.
4. Any other face with an edge whose division changed is re-triangulated
   in its parameter plane, scaled by the surface's speed at the middle so
   it is roughly isometric there: the new boundary (through `edge_uv()`,
   which picks the right side of a closed surface's seam), plus BRepMesh's
   interior points except those within 0.3 of a changed boundary piece's
   length from it. Every other face keeps BRepMesh's triangles.

Points are shared structurally, one mesh vertex per B-rep vertex, edge
point and seam vertex, so each volume's mesh is closed by construction;
`analyse()` confirms it (`mesh.tessellate.open` otherwise). The
tessellation first merges faces on one surface (`UnifySameDomain`, so a
face split by a boolean is whole again) and works on a copy, since
BRepMesh attaches triangulations to the shape and handles are shared.

**Faces on a seam.** A face covering the patch, larger or not, carries it
as above. A face covering only part of the patch, or with an edge inside
it, can't conform: `mesh.tessellate.seam_uncovered` names the surface
and the volume (so does a volume none of whose faces reach the patch).
Bolt holes are seam holes (decision 0007). Where the solid has the hole,
its wall meets the seam's hole loop. Where it doesn't, the face is closed
with the seam's **hole fill**: `make_seam()` now also triangulates each
hole loop on its own (`Seam::hole_fills`), and the weld removes a pair of
hole fills from two volumes facing opposite ways, as it does the seam's
own triangles. So the bracket, whose contents have no holes, welds into
one solid; a blind hole on one side becomes a closed void, as it is.

**Vertices between seam vertices snap to the nearest one** along the
same outline loop, and `mesh.tessellate.snapped` (info) counts them. The
alternative, inserting points into the seam, would break the rule that a
seam is fixed and shared. The mesh moves by at most half a seam piece
there, along the outline. Circles in OCCT start at their frame's x axis
and seams at the outline's, so the two agree whenever the contents are
sketched on the surface's frame; the snapping is for when they don't.

**The combined mesh** (`combine()`): B-rep contents (parametric trees,
STEP and IGES) are tessellated against the seams of the surfaces joining
the volume; static mesh contents (STL, 3MF, OBJ) are read as they are and
fail the weld as non-conforming if a seam joins them; placeholders are
left out, or meshed as their boundaries with `placeholders` set; keep-outs
are never included. Each volume's own closed mesh is kept for per-volume
3MF objects. Nothing is clipped to its boundary or checked against its
neighbours here: that is validation's job (M6, and M9 after generation).

**Mesh files.** STL out is binary; in, binary or ASCII, with corners
merged by identical floats. 3MF is the core spec, written with our own
writer, zipped with the vendored miniz (decision 0006; `vs_mesh` now
links it privately too) and read with a small XML scanner that handles
objects, components, build transforms and units. lib3mf would be a new
vcpkg port, which a cloud session can't add (decision 0005), and the
subset we need is small. Coordinates are written as the shortest decimal
that reads back to the same float, so a 3MF reads back bit for bit, and
the same mesh always gives the same bytes. A volume's id goes in the
object's metadata (`vstudio:volume`). Importing a mesh makes a static
volume whose boundary is its bounding box, between two new datum points.

## Consequences

- `vs_kernel`: `tessellate.hpp` (links TKMesh), and `face_plane()` now
  accounts for left-handed planes: a revolution's planar faces used to get
  their normal backwards.
- `vs_mesh`: `tessellate.hpp`, `combine.hpp`, `io.hpp`, `import.hpp`;
  `Seam::hole_fills`; the weld removes paired hole fills; `triangulate()`
  takes extra constrained segments.
- `vstudio-cli export <project> --stl|--3mf [--per-volume]` and
  `vstudio-cli import <project> <mesh>`.
- Tests: kernel tessellations of boxes, cylinders, spheres (poles), cones
  and filleted boxes join closed by structure alone; blocks, cylinders,
  cone frustums, hemispheres, filleted blocks and bolted flanges (holes in
  neither, both or one side) on plates weld into one shell of the right
  volume; 45 random rigid motions of curved pairs up to 400 mm from the
  origin weld (a one-off run of 1,500 with another seed also passed); the
  bracket exports a single watertight, manifold 3MF of exactly 48,000 mm³
  that reads back bit for bit (and an independent Python check of the
  file agrees); the sample STL imports with 20 triangles and its bounds.
- Not checked here: opening the exported 3MF in a slicer, which the plan
  asks for once, by hand.

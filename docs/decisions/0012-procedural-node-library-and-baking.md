# 0012: Procedural node library and baking

Nov 19, 2026 · M12

## Questions

1. How do lattices and TPMS stay deterministic across platforms when
   they need `sin` and `cos`, which CLAUDE.md says may differ in the last
   bit?
2. Instancing asks for "a mesh template or a small parametric template at
   frames". Generation jobs run on the pool, where the OCCT kernel isn't
   safe to call. What is a parametric template inside a graph?
3. Where do mesh booleans fit, given that a volume's contents are a field
   meshed against its seams?
4. How do published outputs get values: measured ones (mass, volume,
   centre of gravity, envelope) and numbers the graph computes (counts)?
5. What does baking store, so that unbaking gives back the same content
   hash, and does a baked volume still weld?
6. Does the 100 mm gyroid at 0.5 mm finish in a reasonable time on the
   CPU?

## Decisions

**Our own sine and cosine.** `sin_det()` and `cos_det()` (`field.hpp`)
are fdlibm's polynomial kernels with a three-part Cody–Waite reduction by
π/2: plain additions and multiplications, so with FMA off they give the
same bits everywhere. They agree with libm to within 1e-12 over ±20,000 rad
(tested). TPMS fields and circular patterns use them; nothing in a field
goes through libm.

**Infill families are fields.** `lattice_field()` (cubic, BCC, octet
struts) is the distance to the nearest strut of the cell the point lies
in, scaled by the cell size, less half the strut thickness;
`tpms_field()` (gyroid, Schwarz P, diamond) is |f| over a floored
gradient length, less half the sheet thickness, so the thickness is close
to millimetres. Both take an optional grade field that replaces the
thickness pointwise (clamped at zero), which is how `graded_lattice`
thins its struts with depth (`query.distance_to_boundary`). Thickness is
limited by the node (strut < half a cell, sheet < a quarter) so the
result stays a lattice. `field.thicken` (both sides of a surface field)
and `field.clamp` join the M10 offset and shell.

**Frames are FP32 transforms; scatter is seeded and checks itself.**
`scatter.volume` draws points from the volume's derived seed by
`uniform(seed, index)`, keeps those inside the boundary less a margin and
at least `spacing` from earlier ones (a spatial hash), and gives up as
`proc.node.infeasible` after a bounded number of draws, never a hang.
`scatter.surface` draws over the patch, weighted by triangle area, with
frames facing along the surface normal. Both read the volume's inputs as
well as their own, so they aren't cached by node (like input nodes).
Patterns (linear, circular, grid) multiply frames; every frame node is
capped at 100,000 frames.

**A parametric template is a field template.** Generation can't call the
kernel, and a template made of primitives and booleans is what a "small
parametric template" is in practice. So `field.instances` places a
template field at every frame: the minimum over frames of the template
evaluated at the point brought into each frame (inverse rotation stored,
with a reach radius so far frames are skipped). `mesh.instances` copies
a mesh template the same way. A B-rep template would need a kernel job
on the calling thread; that waits for a real need.

**Meshes are tools inside a graph; contents stay a field.** Mesh nodes
(`mesh.box`, `mesh.cylinder`, `mesh.instances`, `mesh.measure`,
`mesh.union` / `.difference` / `.intersection`) work on meshes inside the
graph. The booleans go through Manifold (vcpkg feature `proc`, linked
privately; no Manifold type leaves `src/`), which rejects inputs that
aren't closed manifolds (`proc.node.infeasible` naming the input).
Manifold orders its output by run ids from a process-wide counter, so the
result is put in a canonical order (each triangle from its lowest
corner, triangles sorted, vertices numbered by use) and is the same bits
every time. A mesh becomes contents through `convert.mesh_to_field`, so
the volume is still meshed against its seams and contained; the
`bolt_bosses` template does exactly that with field cylinders.

**Outputs: measured on the mesh, or published by the graph.** The
generator measures the contents mesh as the engine already measured
B-rep contents (`eval::measure_mesh()`, now public): volume, mass
(density × volume), area, centre of gravity, envelope, triangle count. A
new quantity, `OutputQuantity::Published`, takes the value of the
graph's `output.publish` node with the output's name; a missing one has
no value and warns `proc.output.unpublished`. Duplicate publish names are
`proc.graph.publish`.

**Inspection evaluates only what a node needs.**
`EvaluateGraphOptions::only_for` restricts evaluation to a node and its
ancestors, so an unfinished graph can be inspected anywhere it is
complete. `inspect_node()` returns every output with its value, whether
it came from the cache, and diagnostics; `preview()` turns a value into
something drawable: a field becomes a coarse mesh of its region (32 cells
across), frames become points, numbers become text. Node cache keys now
include the volume id and the boundary's box, because mesh nodes put the
volume on their triangles.

**Baking stores the mesh and keeps the graph.** `bake()` writes the
generated mesh as a 3MF blob and turns the volume static
(`StaticContents{blob, "3mf", baked_graph}`); `unbake()` puts the graph
back and drops the blob if nothing else uses it. Everything else on the
volume is untouched, so unbake gives back the same document and the same
content hash (tested byte for byte). The baked mesh still has its seam
vertices: `combine()` re-attaches static meshes' vertices to seams by
exact FP32 position (`attach_seam_vertices()`), so a baked volume welds
as it did before.

**Large meshes: nested pool, fast containment, linear topology.** A
single procedural volume used to generate on one thread, because nested
pool calls ran inline. The pool now keeps a stack of batches; a worker
takes tasks from the newest open one, and a caller works on its own batch
while it waits, so nested calls spread out without deadlocks and results
still fold in chunk order. Mesh containment takes a fast path for convex
boundaries (one lump, planar faces: inside every plane), and otherwise
certifies octree cells whose centre is deep enough inside and classifies
only the rest, in parallel chunks. `mesh::analyse()` buckets half-edges
by vertex instead of hashing them, and surface nets index cubes densely
and keep vertex fans in one array; all of these give the same results as
before, bit for bit.

## Consequences

- Each template generates in a procedural box joined to a parametric
  one, passes containment, welds watertight and manifold, and round-trips
  through 3MF (acceptance 1); bake then unbake restores the content hash
  (acceptance 2). Template meshes are the same with 1 and 3 threads.
- The CPU baseline for the GPU milestone (acceptance 3), from
  `vstudio-cli bench-gyroid --size 100 --cell 0.5` in a cloud session
  (4 vCPUs, Xeon at 2.1 GHz, linux-gcc-release): 8.1 million samples, 9.6
  million triangles, 4.8 million vertices, 415.9 g at 1.24 g/cm³.

  | Stage | 4 threads | 1 thread |
  | --- | --- | --- |
  | Sample | 1.2 s | 4.9 s |
  | Clip to the boundary | 0.5 s | 1.9 s |
  | Surface nets | 15.4 s | 16.1 s |
  | Mesh contents (clip, nets, seams, moves) | 19.9 s | 24.6 s |
  | Containment check | 0.6 s | 0.6 s |
  | Engine total | 23.9 s | 31.7 s |

  Surface nets run on one thread and dominate; they and the per-cube
  merge are the first things to parallelise (by z slabs) or move to the
  GPU. Before this milestone's fixes the same run took 46 s.
- Saved templates live in `libs/vs_proc/tests/data/templates/`, with
  `bolt_bosses.vsproj` as a sample; a test checks they equal
  `make_template()`'s graphs, for M15's "same hashes as their saved
  versions".
- `vstudio-cli templates`, `inspect`, `bake`, `unbake` and `bench-gyroid`
  exercise all of this from the command line.
- Manifold arrived from a cloud session by turning on the `proc` feature
  on the milestone branch and running CI there by hand, which published
  the Linux binary cache (decision 0004) before the first local build.

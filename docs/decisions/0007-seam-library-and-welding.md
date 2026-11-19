# 0007: Seam library, constrained triangulation and structural welding

Nov 19, 2026 · M7

## Questions

1. The plan's discretisation API takes "a seam and a target edge length".
   If each mesher picked its own target, two sides of one surface could
   divide it differently. Whose length is it?
2. "The interior of the patch gets a shared constrained triangulation."
   With what? vcpkg can't add a dependency from a cloud session
   (decision 0005).
3. Which connection surfaces get a seam, and what is their outline: the
   patch alone, or the patch with a bolted flange's holes?
4. What exactly makes a mesh "conforming", and what does welding do with
   the patch's triangles, which both volumes now have?
5. How are seam vertices named so the weld is structural, and how does a
   stale mesh get caught?

## Decisions

**The target edge length is the surface's spacing, and the seam is made
once.** A `Seam` is a pure function of (surface id, exact outline,
spacing): `make_seam()`. The spacing is `ConnectionSurface::seam.spacing`
when set, otherwise `derive_spacing()`, a sixteenth of the outline's
bounding-box diagonal (at least the minimum feature size). The seam
library (`build_seams()`) makes one seam per surface and every mesher
takes its vertices and triangles from there (`add_seam_vertices()`,
`add_seam_face()`); none discretises a seam itself. A per-mesher target
would make conformity depend on two meshers agreeing, which is the risk
the spec names. Each outline segment is divided into
`ceil(length / spacing)` equal pieces, arcs into at least 16 per turn, so
small bolt holes stay round at coarse spacings. `Seam::loops` keeps where
each exact segment starts, so M8 can match B-rep edges to seam pieces.

**Our own constrained Delaunay triangulator** (`triangulate.hpp`, about
650 lines): incremental Delaunay in a super triangle, with points inserted
in a biased randomized order (rounds of doubling size, each in Z-order,
shuffled by our own splitmix64 so the order is the same everywhere),
Sloan's flip-based edge recovery, even-odd removal of the outside and the
holes, then Lawson flips away from constrained edges. Interior Steiner points sit on a
triangular lattice of the seam's spacing, aligned to the frame's origin,
at least half a spacing from the outline. A square lattice would make
every cell cocircular and the Delaunay flips ambiguous; the triangular one
has no cocircular quads. Orientation and in-circle tests are plain FP64
with relative thresholds. Input is validated first (duplicates,
self-intersections, holes outside the outer loop), so the numerical part
only ever sees simple polygons; a Steiner point outside the domain shows up
afterwards as unused. Speed, release build: a 200 mm square at 0.5 mm
spacing (186k vertices) takes 0.3 s, and a 100 x 0.05 mm slot at 0.01 mm
(70k vertices, long collinear runs, the insertion order's worst case)
0.1 s.

**Bonded and bolted-flange surfaces get seams; clearance surfaces don't.**
Across a clearance the volumes stay `gap` apart and nothing is welded. A
surface that joins one volume and the outside gets a seam too, so the
volume still follows the published division. A bolted flange's outline
includes each bolt's ISO 273 clearance hole as a circular hole loop: the
holes go through the face both volumes share, so both must mesh round
them identically. Outlines are read from the built patch with the new
`kernel::face_loops()` (lines and arcs, exactly; any other curve is
`mesh.seam.curve`).

**Conforming means having the seam's triangles; welding removes them in
pairs.** Each volume's mesh is closed on its own, including its copy of the
patch. `weld()` merges vertices only by seam vertex id, then removes every
seam triangle that two different volumes have facing opposite ways: that
is where they are bonded. What remains is one closed shell, watertight and
manifold by construction, and the half-edge checks confirm it. A volume
conforms to a seam that joins it when it has every triangle of the seam's
triangulation, once, all facing one way. Other triangles over three seam
vertices (a ring round a concave outline, outside the patch) are the
mesher's own business. A non-conforming mesh fails with
`mesh.weld.nonconforming` naming the surface first, then the volume; so
do moved seam vertices (`mesh.weld.moved`, positions compared bit for bit
with the seam's FP32 copy), and a volume using a seam that doesn't join it
(`mesh.weld.not_joined`). Open edges, non-manifold edges and orientation
problems name the seam when all their corners are on one, otherwise the
volumes.

**Seam vertex ids derive from a hash of the inputs.** The base id is the
BLAKE3 hash of the surface id, frame, spacing and every vertex's FP64
position; vertex k is `Id::derive(base, k)`. Changing the spacing or the
outline gives new ids, so a cached mesh made against an older seam fails
structurally instead of welding to the wrong vertices. Arc points come
from `cos`/`sin`, so ids may differ between platforms in the last bit of
an arc point; seams are always regenerated, never saved, so that's fine,
and no golden test hashes them.

**Meshers stay in the tests until M8.** The plan's M7 is the seam library
and welding; B-rep tessellation is M8. The property tests use small
meshers (prisms with independent cap spacings, pyramids, a box with the
patch as part of a larger face, a three-volume chain across two seams).

## Consequences

- Property tests: 500 random seams (outlines with up to four holes,
  random frames up to 1,000 mm from the origin, random spacings), 500
  random prism pairs, 200 cases of mixed meshers, 150 three-volume
  chains, and awkward shapes (0.05 mm slots, combs). Two one-off runs with
  20 to 30 times as many cases and fresh seeds (15,000 welds, 15,000
  seams, 30,000 triangulations each) also passed.
- `vs_kernel` gained `face_loops()`. `vs_mesh` links everything up to
  `vs_validate`; `mesh::view()` hands a mesh to `check_containment()`.
- `vstudio-cli seams <project>` prints each surface's seam.
- M8 builds its B-rep tessellator on `Seam::loops` and `segment_starts`;
  M11 stitches field meshes to the same loops. Neither should call
  `triangulate()` on a seam's outline itself.

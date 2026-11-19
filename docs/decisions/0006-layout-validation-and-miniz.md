# 0006: Layout validation, diagnostics routing and vendored miniz

Nov 19, 2026 · M6

## Questions

1. The plan asks for overlap "reporting both volumes, the overlap region and
   its depth", but doesn't say what depth means for a region that isn't a
   box, or how to keep 50 volumes "well under a second".
2. Undeclared contact is "sibling faces within tolerance": does a touch
   along an edge count, and which pairs are exempt?
3. "Connection type mismatch: per-type parameter rules": the plan gives one
   rule (the flange's minimum wall against the hole pattern). Which others?
4. The containment check must take B-rep and mesh inputs, but mesh types
   are M7's, in `vs_mesh`, which sits above `vs_validate`.
5. The registry's "act here" targets are layout, connection surface,
   feature or node. Where does a static file that sticks out of its
   boundary go?
6. Loops are already rejected by `vs_model` at authoring time, and loading a
   project refuses a file with one. How does a loop fixture validate?
7. Decision 0005 wrote stored-only zips because vcpkg couldn't fetch miniz
   in a cloud session. Can it now?

## Decisions

**Depth is the overlap region's thinnest side.** The overlap is
`common(a, b)` in FP64, fuzzy at the linear tolerance. Its depth is the
smallest side of its bounding box: for the box-like overlaps layouts make,
how far one volume has to move to clear the other. A region no deeper than
the tolerance is touching (spec: "penetration up to the tolerance counts as
touching"). Exact penetration depth for arbitrary shapes isn't worth its
cost before a user has asked for it.

**Boxes first, booleans only when they can matter.** Pairs whose bounding
boxes don't meet are never intersected. A pair whose boxes meet only in a
slab no thicker than the tolerance (neighbours that touch) can't overlap
deeper than that, so it skips the boolean too. Contact then measures the
pair's distance and pairs only faces whose boxes meet; planar faces must
face each other on one plane before the kernel computes their shared area
(`kernel::common_faces`, new). A 10 x 5 grid of 50 touching blocks
validates in about 0.23 s on the cloud machine (release build).

**Contact is over an area; edge touches get one warning per pair.** Each
pair of faces lying against each other over more than the minimum feature
size squared is `validate.contact`, naming both faces. Face names come from
the boundary's named faces (`VolumeBuild::boundary_faces`, now kept on the
build): `<feature>:<name>` or `-x`..`+z`, matched by plane or cylinder, or
"face <k>" when nothing matches (a keep-out cut). Two volumes that touch
only along an edge or at a point get one `validate.contact.edge` warning,
because they would still make a non-manifold print. Pairs sharing a
connection surface skip the contact test entirely (structural adjacency),
and so do pairs with a keep-out volume, since touching a keep-out is its
purpose. Overlap applies to every pair, keep-outs included.

**Connection rules.** Per surface, naming the surface and the parameters
in conflict:

- every type: the patch must reach each joined volume's boundary (within
  the tolerance, or the gap for a clearance), `validate.connection.off_boundary`;
- clearance: the gap must be at least the minimum feature size;
- bolted flange: the minimum wall must be at least the minimum feature size;
  each ISO 273 clearance hole must sit inside the patch with the wall to the
  patch's edge (`edge_wall`), and neighbouring holes must leave the wall
  between them (`hole_spacing`).

`model::build()` now evaluates the gap and minimum wall onto
`ConnectionBuild`. The rules report the worst hole or pair once per
surface, not one diagnostic per hole.

**A mesh view instead of mesh types.** `validate::MeshView` is a pair of
spans (FP32 positions, triangle indices), which M7's mesh type can hand
over without `vs_validate` depending on `vs_mesh`. Vertices and triangle
centroids are classified against the boundary with a prepared
`kernel::SolidClassifier` (new; one prepared solid for many points), and a
point counts as outside beyond the linear tolerance plus its own FP32
rounding (2⁻²² of its largest coordinate), because an FP32 mesh can't be
closer to an FP64 boundary than that. B-rep containment cuts the contents
by the boundary and measures how far the remainder's vertices reach.

**A fifth target, Contents.** Routing sends each diagnostic to one of
Layout, ConnectionSurface, Feature (layout or contents, with the volume),
Node, or Contents: a volume's contents as a whole, for containment and
`model.contents.*`, where no feature or node is to blame. The registry
lists every `validate.*` code with its severity and target; other modules'
codes route by their first subject (see `registry.hpp`).

**Validation loads unchecked.** `model::load_project(path, false)` returns
a document even when it breaks the authoring rules, so `vstudio-cli
validate` can report a loop (and every other `model.*` error) instead of
stopping at the first. `validate()` runs `check()` first and doesn't build
a document with errors.

**miniz is vendored, not a vcpkg port.** With the cloud environment's wider
network, `vcpkg install miniz` still fails: port sources come from
GitHub's archive downloads (`github.com/<owner>/<repo>/archive/...`), which
the session's proxy still refuses for repositories other than this one,
while release assets download fine. So miniz 3.0.2's amalgamated
`miniz.c`/`miniz.h` (MIT) are vendored in `third_party/miniz`, like
PlaneGCS, and linked privately by `vs_model`. `.vsproj` zips are now
deflated, and zips from Explorer, 7-Zip or `zip` open. Entries carry a zero
DOS timestamp (`MINIZ_NO_TIME`), so saving stays byte-for-byte
deterministic. `model.project.zip_compressed` is gone. This supersedes the
zip part of decision 0005.

## Consequences

- M9 calls `check_containment()` after each generation and
  `validate_layout()` after each layout rebuild; both take a build, not a
  document, so M9 can reuse its own.
- `vs_kernel` gained `common_faces`, `SolidClassifier`, `distance_to` and
  `vertex_points`.
- The sample bracket validates clean (`vstudio-cli validate`), as does the
  `clean` fixture; the other fixtures live in `libs/vs_validate/tests/data`.
- A new third-party library can come in from a cloud session as vendored
  source from a release asset; a vcpkg port still can't.

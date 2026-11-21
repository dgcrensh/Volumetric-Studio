# 0024: H2, design-rule bypasses

Nov 21, 2026 · plan `docs/plans/H-hardening.md` (PR 28), phase H2

## Context

H2 closes the audit's bypasses of containment (rule 5), encapsulation and
identity (rule 1), one-way data flow (rule 6) and structural validity, on
flat, curved (L2) and composite (L3) models. As in H1, every item starts
with a test that fails on `main`; the `[h2]` tests are in a
`test_design_rules.cpp` per suite (`vs_sketch`, `vs_feature`, `vs_model`,
`vs_validate`). Each was run against `main` (`fd64dcd`) with the fixes left
out, and failed there unless the table says otherwise.

## Decisions

**Containment of B-rep contents** measures how far the part outside reaches
over its vertices *and* a tessellation of its edges and faces (deflection a
hundredth of its size, at least the minimum feature size). Vertices alone
miss a bulge: a cylinder or sphere poking out has its farthest point inside
a curved face and its vertices on the boundary. A `mass_properties()`
failure other than "no volume" is `validate.undecided`, as is a part
outside the kernel can't tessellate.

**Containment of meshes** checks indices before anything else (triangles
with no positions are broken, not empty) and treats a non-finite vertex or
centroid slack as `validate.undecided`: a NaN compares false with every
plane, so the convex fast path used to certify it inside. Classification
chunks take a worker (its own `kernel::deep_copy()` of the boundary and a
classifier prepared on it) from a pool and give it back, so no two threads
read one shape and a worker keeps its prepared classifier across chunks.
Results stay per chunk, folded in chunk order. `SolidClassifier` is no
longer copyable (copies shared mutable OCCT state).

**Nesting** skips the boolean only when the parent is exactly its box: one
solid of six planar faces, each spanning one side of its bounding box. The
old test (box volume within the tolerance times the volume) let a 1,000 mm
composite miss up to 1,000 mm³, a 10 mm notch.

**Connection surfaces.** `validate.connection.off_boundary` also fails a
patch with a point inside a volume it joins farther than the reach (the
tolerance, plus the gap for a clearance) from that volume's surface: a
patch floating inside, or poking through. Points are the patch's vertices,
its tessellation's points and its triangles' centres put back on the
surface in (u, v) (a rectangle's mesh is its outline alone). A point
*outside* the volume is left to the patch's distance to the solid, as
before, so a patch may still run past the face it lies on (the 50-volume
performance layout straddles joints that way). A **clearance** surface no
longer excuses contact between its volumes: it keeps them apart by its
gap, so touching is still `validate.contact`. Bonded and bolted surfaces
still do.

**Routing.** `validate.undecided` goes where its first subject is: a
volume's contents (containment), a connection surface (connection rules),
or the layout (overlap, contact, nesting). The level of a layout-wide
subject also covers published outputs and connection surfaces (a
composite's).

**Ids.** An authored object's own id may not be null, in files
(`read_own_id()` in the sketch, feature and model readers; references may
still be null and resolve to nothing) and in `check()`
(`model.document.null_id`). `feature::tree_ids()` and `find_tree_id()` are
the one walk over a tree's ids (variables, features, a sketch's own id,
entities and constraints), used by `contents_owner()`, `classify()`,
`check()`'s uniqueness and the dependency graph; constraints weren't in
two of the four walks it replaces. A composite that is its own parent is
`model.volume.parent` (`ancestors()` stopped at it, so the loop check never
saw it).

**Dependency graph.** A feature whose result depends on the body above it
(a through-all extrude, any cut, shell, fillet, chamfer, or pattern)
depends on *every* unsuppressed body feature above it, not only the last:
a blind extrude in between doesn't carry the earlier ones along. Blind
adds still depend only on what they name, so links between volumes built
from independent blind features stay legal. Layout references to a
sketch's own id or constraints, a composite's boundary and its joining
surfaces (a datum patch on the composite's face) now get their edges, as
features' references already did, so L3 waves order correctly.

**Rebuild and build.**

- `feature::rebuild()` reuses previous results only when every feature
  above the start is the one the previous result was built from
  (`RebuildResult::built_from`, compared whole: definitions, sketches,
  suppression) and the `ProductSettings` match (`RebuildResult::settings`).
  A wrong or stale start never reuses a stale body.
- An `ExternalInputs` value never stands in for a tree variable of the
  same name, failed ones included.
- A composite keeps its layout's rebuild (`contents_rebuild`) when it
  fails; a keep-out whose cutter volume has no boundary fails the boundary
  it cuts (`model.boundary.feature_failed`, naming the cutter) instead of
  cutting nothing; a failed volume's partial contents are left out of its
  composite's measured outputs.

**Profiles.** Before nesting, each loop is checked against itself
(`sketch.profile.self_crossing`, a bowtie) and against every loop whose box
meets its own (`sketch.profile.loops_cross`) for sides crossing each other,
by a sweep in x. Two rectangles in a '+' have no corner inside each other,
so the corner test alone passed them. A loop all of whose points lie on
another (coincident circles) is `loops_cross` too. Arcs become polygons
whose chords stray no more than the minimum feature size from them (at
least 256 chords a turn, more on large radii); points within the
tolerance plus that much count as touching. Boxes prune both the pairs and
the points tested.

**No hashes change.** Nothing here is hashed or cached differently;
`kEngineVersion` stays 1 until H3.

**Reproduced and fixed** (each `[h2]` test failed on `main`):

| Item | Failing test (what `main` did) | Fix |
| --- | --- | --- |
| Curved contents bulging out | `Curved contents bulging out of their boundary are caught`: a cylinder 0.4 mm and a sphere 0.5 mm out, both contained (reach 0) | reach over tessellated points |
| NaN mesh vertices | `A broken mesh is undecided, never contained`: NaN or infinite vertex contained; NaN slack ignored; triangles with no positions contained | finiteness and index checks first |
| Notched composite | `A child in a notch of a large composite is outside it`: a child in a 9 mm notch of a 1,000 mm composite passed | exact box-fill test |
| Patch inside or through a volume | `A patch must not float inside or poke through a volume it joins` (nothing reported); `Connection type rules` reported only the volume the patch was off | interior points of the patch |
| Clearance hides contact | `A clearance surface doesn't excuse contact` (nothing reported) | contact skipped only for bonded and bolted surfaces |
| Undecided routed to the layout | `validate.undecided goes where its first subject is`; `Outputs and surfaces inside a composite route to its level` (root level) | routing by first subject; outputs and surfaces in `level_of_subject()` |
| Null ids | `Authored objects can't have the null id` (no finding; files with null volume, surface, output, parameter, link and feature ids loaded); `A sketch file can't give an object the null id` | `read_own_id()`, `model.document.null_id` |
| Constraints left out of uniqueness | `Sketch constraints and sketch ids take part in id uniqueness` (a shared constraint id passed) | `tree_ids()` |
| Self-parenting composite | `A composite can't be its own parent` (only its children were reported) | explicit check |
| Through-all feedback loop | `A through-all extrude depends on every body feature above it`: no loop with a blind extrude between | edges to every earlier body feature |
| L3 wave order | `Surfaces on a composite's boundary face order the waves`: Core's contents didn't depend on Housing's boundary and ran before the volume driving it | `uses_layout()` resolves boundaries and surfaces |
| Stale incremental rebuild | `An edit above the start feature isn't hidden by reuse`: an edited plate, its sketch and other settings all reused | `built_from`, `settings` |
| Inputs shadowing failed variables | `An input value never stands in for a failed tree variable`: the input's 3 mm was used | names of the tree's variables are never filled from inputs |
| Failed composite lost its layout | `A failed composite keeps its layout's rebuild` | kept through `apply_contents()` |
| Failed cutter cut nothing | `A keep-out whose cutter failed fails the boundary it cuts` (boundary kept, Current) | boundary fails |
| Failed contents in composite sums | `A failed child's partial contents don't count in its composite's outputs` (16,000 mm³ instead of 12,000) | measured only when not failed |
| Crossing profiles | `Two rectangles crossing in a '+' cross`, `A bowtie crosses itself`, `Coincident circles are one loop drawn twice` (all made profiles) | side crossings, coincident loops |
| Coarse large arcs | `A hole near a large arc is inside it, not across it`: a hole 0.03 mm inside a 900 mm circle was "crossing" | deflection-driven sampling |
| Quadratic profile nesting | `Many separate loops are compared only where their boxes meet`: 400 circles took 47.6 s; now 0.02 s (passes on `main`, slowly) | per-loop boxes, x sweep |

**Done without a failing test:**

- The octree's "can't split" test compared a child with every sample
  rather than with its parent's group; it only let recursion run to
  `kMaxDepth` sooner or later, never changing an answer. The face-distance
  loop now stops at the first face nearer than the node's radius (speed).
- Per-worker boundary copies: no race reproduced on Linux; done as the
  plan asks, since OCCT doesn't promise that reading one shape from
  several threads is safe. `Mesh containment gives the same answer on any
  number of threads` covers it.

**Tests that changed.** `Connection type rules`: the patch 5 mm inside B is
now off both A and B, and the clearance-gap case also reports the two
volumes' contact. `A band off the shaft is off its boundary`: the band
2 mm off the shaft also lies inside the sleeve's wall, so both are
reported.

**Branch.** Cloud sessions push to their own branch, so H2 is on
`claude/project-thread-n7uvc3` rather than `h2-design-rule-bypasses`.

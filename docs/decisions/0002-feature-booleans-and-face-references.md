# 0002: Feature booleans are fuzzy; face references are found by geometry

Nov 19, 2026 · M3

## Questions

1. Decision 0001 left M3 to decide which features pass the product
   tolerance as the boolean fuzzy value (`BooleanOptions::fuzzy_mm`).
2. The plan asks that references to result faces "store enough to re-find
   them after rebuild", with a simple, documented strategy.
3. A few behaviours the plan leaves open: which way a cut goes, how far
   "through all" goes, what "up to plane" accepts, and where a sketch on a
   face puts its origin.

## Decisions

**Every feature boolean is fuzzy.** Extrudes and revolves, adding or
cutting, combine with the body using `fuzzy_mm = linear_tolerance_mm`, and
so do the unions that join a feature's own profiles. In the authoring layer
two faces within the tolerance are touching by definition (spec), and
features meet that case all the time: a boss sketched on a datum that sits a
rounding error off a face, a cut whose floor lands on an existing face. M1's
spike found that fuzzy booleans keep OCCT's entity tolerances at 1e-7 mm,
and the rebuild runs `audit_tolerances()` after every feature anyway, so a
case that grows them shows up as a `kernel.tolerance.exceeded` warning on
the feature. `vs_feature_tests "[fuzzy]"` checks that a boss half a
tolerance above a face joins it into one solid.

**Face references are found again by geometry.** A `FaceRef` names a
feature and a face name that feature gives:

| Feature | Names |
| --- | --- |
| Extrude (add) | `start`, `end`, `side:<sketch line id>` |
| Extrude (cut) | `end` (floor of a blind or up-to-plane cut), `side:<sketch line id>` (walls) |
| Revolve (partial) | `start`, `end` |

On every rebuild the named feature reports where each named face lies (a
plane and outward normal, computed from its sketch and parameters). The
reference resolves when the body, as the referring feature sees it, still
has a planar face on that plane facing the same way. Consequences, all
covered by tests:

- References follow parameter changes (a sketch on an extrude's `end` moves
  when the depth variable changes).
- They survive later features that split or trim the face (a hole through
  the face).
- They break with `feature.reference.broken` when a later feature removes
  the face entirely, or when the sketch line that made a side face is
  deleted.

Only planar faces are named in M3, because that is all sketches, datums and
"up to plane" need. M4 extends the names to its features (and to curved
faces if fillets or shells need them). The spec accepts this fragility
inside a volume; connection surfaces never depend on it.

**Smaller behaviours.**

- A cut goes against the sketch plane's normal (into the material under a
  sketch on a face) and an added extrude goes along it; `reverse` flips
  either.
- "Through all" goes 1 mm past the body's bounding box in the extrusion's
  direction, clipped to the working space. With no body it is an error.
- "Up to plane" accepts only planes parallel to the sketch plane
  (`feature.extrude.up_to_not_parallel`). Up to a slanted plane or a curved
  face can come later if a workflow needs it.
- A positive draft tapers the profile inwards along the extrusion. With a
  symmetric extent it tapers both ways from the sketch plane.
- A sketch on a face or other plane known only by a point and a normal gets
  the frame from `frame_for()`: its origin is the projection of the model
  origin, and its x axis is the model axis most perpendicular to the normal,
  projected. Principal planes have fixed frames (`principal_frame()`).
- Rebuilding from a changed feature reuses the results above it only when
  the tree above it and the variables' values are unchanged; otherwise it
  rebuilds everything. The M9 scheduler will build on this.
- Expressions accept units (mm, cm, m, um, in, ft, deg, °, rad) that convert
  into millimetres and radians, but do no dimensional analysis.

## Consequences

- The kernel gained `face_plane()` and `make_drafted_prism()` (OCCT's
  `BRepOffsetAPI_DraftAngle`, linking TKOffset and TKFeat).
- M4's sweep, loft, shell, pattern, fillet and chamfer follow the same
  rules: fuzzy booleans, named planar faces, the tolerance audit.

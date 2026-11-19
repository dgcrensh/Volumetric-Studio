# 0003: M4 features, edge references and kernel fallbacks

Nov 19, 2026 · M4

## Questions

1. Decision 0002 left M4 to extend face references to its features, and
   fillets and chamfers need a way to name edges.
2. The plan asks for "validation and clear fallback errors for the OCCT weak
   spots (fillet and shell failures)", without saying what "fallback" means.
3. A few behaviours the plan leaves open: how a sweep path is drawn and
   which way it runs, what loft sections may contain, what patterns may
   copy and how their count and angle are read.

## Decisions

**Edges are found by the faces they bound.** An `EdgeRef` is a `FaceRef`
plus an optional second `FaceRef`: the edges where the two faces meet, or
every edge of the one face (seams excluded). Both faces are found by
geometry exactly as in decision 0002, so edge references follow parameter
changes and survive splits, and break with `feature.reference.broken` when
either face is gone or they no longer meet. No edge ids are stored.

**Curved faces get names too, but only cylinders.** An extrude's side face
made by a sketch circle or arc is named `side:<curve id>` (without draft;
with draft it is a cone, left unnamed) and described by its axis, radius and
which way it faces. That is what a fillet round a hole's rim needs. Sketches
and datums still need planar faces; a cylindrical one is
`feature.reference.wrong_kind`.

**New names.** Sweeps name their `start` and `end` caps (`end` only when
the path starts on the profile's plane, because only then is the end cap's
plane known to pass through the path's end). Lofts name the first and last
sections `start` and `end`. Pattern copies carry the seed's names as
`<k>:<seed id>:<name>`, k = 1 to count − 1, so a feature can sit on a copy.
Shells, fillets and chamfers name nothing: they trim earlier faces rather
than replace them, so references to those keep working (an outward shell
does turn the original faces into the inside, and references to them
break, correctly).

**Fallbacks.** For the OCCT weak spots the rebuild does three things:

- *Validate up front where OCCT doesn't.* An inward shell thicker than a
  convex cylinder's or sphere's radius (or an outward one thicker than a
  concave one's) makes OCCT return a valid-looking solid with the face
  turned inside out. The kernel rejects it first with
  `kernel.shell.too_thick`.
- *Never return a bad solid.* Every sweep, loft, shell, fillet and chamfer
  result goes through the kernel validity check; failures are
  `kernel.<op>.failed` with a message that says what to try.
- *Search for a value that works.* When a fillet, chamfer or shell fails in
  the kernel, the rebuild halves the value up to three times until one
  works, then bisects three times, and reports roughly the largest working
  value in the error and as a `feature.fallback.works` info diagnostic. At
  most six extra kernel calls, only after a failure.

The failed feature leaves the body as it was and the features below carry
on (M3's rule).

**Smaller behaviours.**

- A sweep path is a whole sketch: its non-construction lines and arcs must
  join end to end into one chain (`feature.sweep.path_branches`,
  `.path_disconnected`, `.no_path`), or be a single circle. An open path
  runs from the end nearer the profile's centre. Orientation is
  `follow_path` (OCCT's corrected Frenet frame) or `fixed`.
- Loft sections are whole sketches with exactly one profile and no holes
  (`feature.loft.section_not_single`); cut holes afterwards. `ruled` is the
  only option; no guide curves yet.
- Shells need at least one open face. Outward shells use the intersection
  join, so their outer edges stay sharp.
- Patterns copy extrudes, revolves, sweeps and lofts (anything that records
  the solid it added or cut), each applied as the original was. A pattern
  can't copy fillets, chamfers, shells or other patterns
  (`feature.pattern.not_solid_feature`). `count` includes the original,
  must be a whole number from 1 to 1,000, and is usually an expression. A
  circular pattern with a full turn spreads copies evenly; with less, the
  last copy lands on the angle. All copies of one feature go into one
  boolean (the kernel's booleans now take several tools).

## Consequences

- The kernel gained `make_sweep()`, `make_loft()`, `shell()`, `chamfer()`,
  multi-tool `unite()` and `cut()`, `face_cylinder()` and `is_seam()`.
- `FeatureResult` records each solid feature's tool and operation, and
  `NamedFace` may describe a cylinder.
- `vs_feature_tests` covers each feature on representative parts and every
  failure listed here; `tests/data/tray.vfeatures.json` is a sample M4 part
  for `vstudio-cli build`.

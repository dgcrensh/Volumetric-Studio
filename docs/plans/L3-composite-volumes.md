# L3 — Composite volumes and deep nesting (plan)

Nov 20, 2026 · follows L2 · decision record: `docs/decisions/0021-composite-volumes.md`

**Status: built.** As below, with three differences found while building it
(decision 0021 has the details): "Met by" is a choice on the connection
surface's menu rather than in the inspector, one entry for each composite
the surface joins; re-routing a surface drops the inner composites' onward
passes that no longer receive it; and `route()` names the composite
(`ActTarget::volume`) for every layout-level diagnostic inside one, not
just for the new nesting check. A checked-in sample,
`libs/vs_model/tests/data/nested.vsproj`, is the plan's three-level
document with every leaf filled.

## Goal

Let a volume's contents be **child volumes**, authored at any depth, so a
design can be broken down the way the spec describes it: a tree of
volumes. A gearbox housing becomes a composite of a shell, a bearing
carrier and a cover; the bearing carrier can itself be a composite. Every
rule that holds at the root holds at each level: the composite is the
common parent of its children, so it owns the connection surfaces and
keep-outs between them; siblings may not overlap; nothing outside a
composite may refer to its children; contents stay inside their
boundaries; and the combined mesh still welds structurally.

The spec's table already describes the kind: "Composite: child volumes,
authored or generated; regenerates when any child or layout input
changes; edited by its own layout". L3 is the authored half; children
emitted by procedures are L4.

## The model: a composite's contents are its own layout

A fourth kind of contents, `CompositeContents`:

- **its layout**: a `FeatureTree`, rebuilt exactly as a parametric
  volume's tree is, with the composite's boundary, the connection
  surfaces joining it and its parameters as external inputs. Its sketches,
  datums and solid features are what its children's boundaries,
  connection surfaces and keep-outs are built from. Its variables drive
  its children's parameters. So the composite is to its children what the
  root layout is to the top-level volumes, and it sees nothing of its own
  parent: the same "contents see inputs, nothing else" rule as today.
- **delegations**: for each connection surface that joins the composite
  from outside, which child meets it (`{surface, child}`). The surface is
  an input to the composite; the composite passes it down to the child
  that sits against it, and that child takes it as a fixed input too.
  A delegated child may itself be a composite and delegate further. A
  surface is delegated to at most one child (a seam split between two
  children is out of scope).

**Where the children live.** `Volume` gains `parent` (null at the root),
and every volume stays in the document's one flat list, as do connection
surfaces, keep-outs and links. A connection surface, keep-out or link
belongs to the level of the volumes it names (they must be siblings), and
its geometry refers to that level's layout: the root layout, or the
composite's. This keeps ids unique across the document as they are
today, keeps every existing loop over `volumes` meaningful (each volume
is still one node with inputs and contents), and makes a flat document's
JSON byte for byte what it is now (`parent` is written only when set).
A tree inside `CompositeContents` was the alternative; it would have
meant recursing through every module that walks volumes for the same
result.

Only a composite may have children; a keep-out volume can't be a
composite. Moving a volume to another level is not an edit L3 offers
(its references would all be at the wrong level).

## Encapsulation at every level

`Scope` already means "the root layout" or "inside volume V". Inside a
composite C is C's level, so the rules generalise without new scopes.
From the level of C (or the root):

- allowed: C's own layout (features, sketches, variables), C's boundary,
  the surfaces joining C, C's parameters, and everything a level owns:
  its children (their boundaries, parameters and published outputs) and
  the surfaces, keep-outs and links between them;
- `model.reference.contents`: anything inside a child's contents,
  including grandchildren and their surfaces (as today for contents);
- `model.reference.parent`: the parent's layout, or anything of the
  parent's level (as today from inside a volume);
- `model.reference.sibling`, `.not_input`: as today, at every level.

New structural checks (`check()`): `model.volume.parent` (the parent
isn't a composite, or the parents loop), `model.level.mixed` (a surface,
keep-out or link names volumes of different levels),
`model.composite.delegation` (a delegation names a surface that doesn't
join the composite or a volume that isn't its child, or a surface twice),
and the warning `model.composite.undelegated` (a bonded or bolted surface
joins a composite but no child meets it: the weld will refuse it, but
blocking it at authoring would stop a user blocking out top-down).
Composite outputs are Volume, Mass, Centre of gravity and Envelope
(`model.volume.bad_output` for Surface area and Published, which don't
add up over children).

`joins(document, surface, volume)` (new, `hierarchy.hpp`) is true when
the surface names the volume or is delegated down to it, and
`seam_sides(document, surface)` gives the volumes whose contents actually
meet it (delegations followed to the end). Everything that today asks
`surface.joins(volume)` for contents' inputs (parametric inputs, cache
keys, procedural inputs, seams, containment slack, the dependency graph)
asks the model-wide `joins()` instead. With no composites the two agree.

## Build, validation and evaluation

- **Build** (`vs_model`). `build_layout()` works level by level, parents
  before children: the root layout, its volumes' boundaries, keep-outs,
  surfaces and parameters; then each composite's layout (with its inputs,
  as for a parametric tree; kept in its `VolumeBuild::contents_rebuild`),
  and from it its children's. Lookups (features, points, planes, axes,
  variables in expressions) go to the level's layout. A composite's
  contents are its children: `build_contents()` makes no solid for it,
  and its outputs add its children's measures up (`build()` and the
  engine both). A failed composite layout fails its children's
  boundaries with the usual `model.boundary.*` codes.
- **Validation** (`vs_validate`). Overlap and contact are checked between
  siblings only, at every level. New: `validate.nesting.outside` (error),
  a child's boundary reaching outside its composite's boundary by more
  than the tolerance, with depth, routed to the child's boundary (its
  composite's layout is where to act). Connection rules also check the
  patch against each delegated child's boundary. Containment of contents
  is checked per leaf, as now; with children inside their parent, that
  is containment all the way up.
- **Engine** (`vs_eval`). A composite is a node whose upstream is its
  children: it is never generated or cached; once its children are done
  its status is the worst of theirs (failed, then stale, then current),
  its outputs are their measures added up, and its key is a hash of its
  children's keys. Links work at every level (`apply_links()` writes
  into a composite's layout variables too), in waves as now. Swapping
  one child changes only that child's key and its ancestors'.
- **Meshing and procedures.** Seams, procedural inputs and the weld
  follow `joins()` and `seam_sides()`; `combine()` tessellates leaves
  only. A seam between a top-level volume and a grandchild welds the same
  as one between siblings, because both sides take the same surface
  object.
- **Exchange.** STEP export names each part by its path ("Housing /
  Carrier / Race"); 3MF per-volume objects likewise.

## App

- The volume tree nests children under their composite, with the
  composite kind and an aggregate status.
- Swap offers "Composite" (from a placeholder; a composite with children
  can't be swapped away).
- **Edit in context** on a composite edits its layout: the feature tree,
  sketch mode and feature dialogs all work on it as on a parametric
  volume's tree, and Add Volume, Add Connection Surface and Add Keep-out
  add at that level. Reference lists and picking offer only that level's
  things. Everything outside the composite is dimmed, as now.
- The connection-surface inspector of a surface joining a composite has
  "Met by" (the delegation), listing the composite's children.

## Tests (acceptance)

1. A three-level document (root → composite → composite → leaves)
   round-trips through JSON exactly; a flat document's JSON, content hash
   and every golden hash are unchanged.
2. Encapsulation: each refusal above, from each level, with the
   expected code; delegation and level checks; dependency loops through
   a composite's layout variable are found.
3. Build: children's boundaries follow their composite's layout and
   parameters; a change to the composite's boundary moves them.
4. Validation: sibling overlap inside a composite, a child poking out of
   its composite (depth reported), contact only between siblings, a
   delegated surface off its child's boundary.
5. Weld: B-rep and procedural leaves at different depths, joined through
   delegated surfaces, weld into one watertight, manifold mesh of the
   right volume; containment passes.
6. Engine: editing a grandchild regenerates only it; the composite's
   outputs are the sum of its children's; a link inside a composite runs
   in waves; swapping a child leaves every non-ancestor key unchanged.
7. App: the outline nests; a composite is built in context through the
   same commands as the root (layout sketch, child volumes, a surface
   between them, a delegation), undo restores identical hashes.
8. Every existing test, golden hash and sample project is unchanged.

## Out of scope

Procedures that emit child volumes (L4); moving a volume between levels;
a seam split across several children; transforms or instancing of
composites (a composite is authored in place, in model coordinates);
composite published outputs other than the four that add up.

# 0021: Composite volumes and deep nesting

Nov 20, 2026 · L3 · plan: `docs/plans/L3-composite-volumes.md`

## Questions

1. How does a document hold volumes at any depth, so that a flat
   document (every MVP project, sample and golden hash) stays exactly what
   it is?
2. What does a composite's contents hold, and what are its children built
   from?
3. Encapsulation was "the layout" or "inside one volume". What may a
   child, a composite and the root each refer to?
4. A connection surface joins a composite from outside. The composite has
   no contents of its own to meet it with. Who does, and how does the weld
   find them?
5. What do overlap, contact and containment mean across levels?
6. How does the engine schedule a composite, and what are its status,
   key and outputs?

## Decisions

**One flat list, with parents.** `Volume::parent` names the composite a
volume is a child of (null at the root), and every volume stays in
`Document::volumes`, as do connection surfaces, keep-outs and links. A
surface, keep-out or link belongs to the level of the volumes it names
(`model.level.mixed` when they differ), and its geometry and expressions
are built from that level's layout. A tree inside the composite's
contents was the alternative; every module that walks volumes (the
engine, seams, the weld, export, the app) would have had to recurse to
reach the same place, and ids are already unique document-wide, which a
flat list keeps trivially true. `parent` is written to JSON only when
set, so flat documents, their content hashes and every golden value are
unchanged. Only a composite has children (`model.volume.parent`, which
also catches a loop of parents); a keep-out can't be one.

**A composite's contents are its own layout.** `CompositeContents` holds a
`FeatureTree` and its delegations. The tree is rebuilt exactly as a
parametric volume's is, with the composite's boundary, the surfaces joining
it and its parameters as external inputs (`feature::ExternalInputs`), so
it sees nothing of its parent: "contents see inputs, nothing else" holds
for composites too. Its children's boundaries, the surfaces and keep-outs
between them and their parameters' expressions are built from it, which
makes a composite to its children what the root layout is to the top
level. `Volume::tree()` returns it, so everything that edits a tree in
context (feature commands, dialogs, sketch mode, the variables table)
works on a composite's layout unchanged. `build_layout()` goes level by
level, parents first: the root layout, its volumes, then each composite's
layout (kept in its `VolumeBuild::contents_rebuild`; errors fail the
composite with `model.contents.failed`) and its children. The level's
lookups also reach the composite's inputs, so a child's corner can be a
point on a face of the composite's boundary.

**Scopes generalise without new kinds.** "Inside a composite" is its
level. From a level: its own layout, its own boundary, the surfaces joining
it and its parameters; and what the level owns, its children's
boundaries, parameters and published outputs and the surfaces, keep-outs
and links between them. Anything deeper (a grandchild, a child's
contents) is `model.reference.contents`; anything of a level around it is
`model.reference.parent`; a neighbour is `model.reference.sibling`, as
before. A link's target is a variable of the level's layout or a child's
parameter; a composite's own parameters are driven from outside it. From
the root these are exactly the MVP's rules.

**Surfaces are passed down, by the composite.** A surface joining a
composite is one of its fixed inputs; a `Delegation {surface, child}` in
its contents says which child lies against it, and that child takes the
same surface object as a fixed input (a composite child may pass it on).
The choice is the composite's: it is about its contents, which nobody
outside may see. `model::joins()` (`hierarchy.hpp`) is "names the volume,
or is delegated down to it", and everything that gathers a volume's
inputs (parametric trees, procedures, cache keys, seams, containment
slack, the dependency graph) uses it. `seam_sides()` follows each named
volume's delegations to the end; that is what a seam's `volumes` hold,
so the weld pairs, say, a top-level volume with a grandchild through one
surface, structurally, exactly as it pairs siblings. One child meets a
surface (a seam split between two children is out of scope). A
seam-bearing surface no child meets is a warning, not an error
(`model.composite.undelegated`): a composite is usually blocked out
before its children exist, and an error would stop every edit in
between. Such a surface meets nothing inside, and the two sides stay
separate shells, as with no surface at all. Re-routing a surface in the
app drops the inner composites' onward passes that no longer receive it.

**Validation is per level.** Overlap and contact are checked between
siblings only: a child is inside its parent, and meets its parent's
neighbours only through delegated surfaces. New, `validate.nesting.outside`
(error): a child's boundary reaching outside its composite's, with depth
and region as for overlap (skipped by a box test when the parent fills
its box). It routes to the layout, with `ActTarget::volume` naming the
composite whose layout holds the child's corners; `route()` now fills
`volume` for every layout target at a composite's level. A delegated
surface must lie on each child it is passed to
(`validate.connection.off_boundary`). Containment stays per leaf; with
children inside their parents that is containment all the way up.

**A composite never generates.** In the engine its children are upstream
of it (the dependency graph's composite contents use each child's), so it
is reached after them in the same wave loop. Its status is the worst of
theirs (Failed, then Stale, then Current; Current with no children); its
key hashes its own definition (layout and delegations) and its children's
keys, so a change anywhere below changes it and nothing else; it isn't
cached. Its outputs add its children's measures up (`ContentsMeasure`:
volume, volume-weighted centre, united envelope; meshes measured with
`measure_mesh()`), which is exact because siblings never overlap. Volume,
Mass, Centre of gravity and Envelope only: surface area and published
numbers don't add up over children (`model.volume.bad_output`). Links
work at every level; `apply_links()` writes composite layout variables
too. `combine()`, STEP export and 3MF take leaves only; STEP parts are
named by path ("Housing / Left / Core").

**The app works at a level.** The volume tree nests children under their
composite. Swap offers Composite (refused away from one that still has
children, `app.swap.has_children`). Edit in context on a composite edits
its layout and makes it the authoring level: new volumes, surfaces,
keep-outs and links go there, their dialogs list only that layout's
features and that level's volumes, and the composite's descendants stay
undimmed (`Viewport::set_context(..., inside)`). A surface's menu has
"Met inside <composite> by" for each composite it joins. Deleting a
composite deletes everything inside it.

## Measured

The nested sample (`libs/vs_model/tests/data/nested.vsproj`: root →
Housing → Left → Core and Cap, plus Right and Base; Seat passed from the
root to Core two levels down, Middle from Housing to Core) builds, welds
into one closed manifold mesh of the right volume with B-rep leaves and
with a procedural Core, and gives the same output hash on 1 and 4
threads. Editing Cap regenerates only Cap; the keys of Base, Core and
Right don't change, Left's and Housing's do.

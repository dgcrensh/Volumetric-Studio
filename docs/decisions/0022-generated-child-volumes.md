# 0022: Procedures that emit child volumes

Nov 20, 2026 · L4 · plan: `docs/plans/L4-generated-child-volumes.md`

## Questions

1. Where does a procedure that makes children live, and what does it make?
2. Where do generated children live, so that every L3 rule (encapsulation,
   overlap, nesting, containment, the weld) holds for them unchanged?
3. What are generated children filled from, and how does one template fill
   boxes in different places?
4. When does the generator run, and what happens when it fails or its
   layout fails a check?
5. What changes for documents without generators?

## Decisions

**A composite may have a generator.** `CompositeContents` gains
`generator` (a vs_proc graph, opaque JSON in vs_model as procedural
contents are) and `templates`. The spec calls a composite's children
"authored or generated"; L3 built the authored half, so the generator is a
member of the same contents, not a new kind: a composite can have both
authored and generated children, and they are siblings. Both members are
written to JSON only when set, so every L3 document, its content hash and
its composites' cache keys are unchanged.

**It makes a layout, as plain data.** `model::GeneratedLayout` holds
children (a key, a name, a template name, an axis-aligned box, parameter
values, a seed), surfaces between two of them (bonded or clearance, a
rectangle on an axis-aligned plane) and delegations of surfaces that join
the composite. vs_model can hold it without knowing graphs; vs_proc makes
it (a new port type, `Children`, appended so existing value hashes stay;
nodes `children.grid`, `.at_frames`, `.merge`, `.delegate`, and
`output.children` as the generator's one output node). Boxes only: they
are what a procedure can place exactly without a kernel, and they map onto
`BoxBoundary` and datum patches, which the model already builds and
checks. A grid's joins are inset from the shared face's edges (`margin`,
1 mm) so no two seams share an edge, as the L3 sample's surfaces are.

**Generated children are written in as if authored.** `model::expand()`
appends datum points and planes to the composite's layout and child
volumes with box boundaries, datum-patch surfaces and delegations, all
with ids derived from the composite's and the generator's key
(`Id::derive`), so a child keeps its id and its cache entry while the
procedure keeps making it, and the same layout always expands to the same
document. Generated volumes are appended after the authored ones (authored
indices don't move), carry `Volume::generated`, and exist only in the
evaluated document: nothing authored can name them, which is "cannot be
referred to from outside" by construction, and the expanded document goes
through `check()` like any other. Two rules relax for them: their names
(and their surfaces') needn't be distinct, since a generator names by
pattern and they are found by id.

**Templates are copied with fresh ids and an offset.** A `ChildTemplate`
holds static, parametric or procedural contents, parameters and outputs;
not a composite, which keeps generation one level deep per composite (and
keeps the types acyclic). A parametric template's tree refers to its
boundary by the template's own id, as a box volume's tree does; a copy's
ids are all derived from the child's (by rewriting every 32-hex id in the
tree's JSON, including inside pattern face names), the template's own
becoming the child's. Sketches are placed in model coordinates, so a
template authored for one box would fill every copy in the same place:
`ChildTemplate::origin` is the min corner of the box it was authored for,
and a copy carries `Volume::contents_offset` (its box's min corner minus
the origin). `build_contents()` rebuilds the tree with every input moved
by -offset and moves the result by +offset. Procedural templates need no
offset: graphs read their boundary. The easiest template is an authored
child: **Make template** keeps its id (so its tree's references hold) and
its box's min corner as origin.

**The generator runs first, once per key.** `Engine::evaluate()`, after
`check()`, builds the layout once (only when some composite generates),
asks the layout generator (`set_layout_generator()`; `proc::install()`
sets the procedural generator, which serves both roles and shares its node
cache) for each generating composite, caches the outcome by the
composite's key, expands it and checks the result. From there the L3
engine runs unchanged on the expanded document. A generator that fails,
or whose expansion `check()` refuses, leaves the composite with no
generated children and Failed with the diagnostics; with no layout
generator it is Stale (`eval.generated.no_generator`). Children that fail
a layout check with an error (overlap, nesting, connection rules) fail
their composite (`eval.generated.layout`): the spec's "must pass the same
checks" is a status, not only a list. Containment stays per leaf.
vs_validate sits below the engine, so `validate::validate()` (and
`vstudio-cli validate`) checks the authored document only; the app's
export check also reads the shown evaluation, where a composite whose
generated layout failed is Failed, so export stops there.

**Links can't drive a generator.** The generator runs before links carry
values, so a link from outside into a generating composite's inputs would
be silently ignored. `check()` refuses it
(`model.composite.generated_link`); links inside the composite, between
its children, are fine. Running generators in the wave loop, with the
document growing between waves, is left for later.

**The app treats generated children as read-only.** The volume tree nests
them ("Generated Parametric"); their menu says which procedure made them
and opens it; edit in context is refused. A composite's menu offers
Generate children / Edit generator (the Graph dock, with the template
property a list of the composite's templates, and a Children preview
drawing the boxes), Stop generating children, and Make template on its
authored children. Every change is a Command, so undo restores identical
hashes.

## Measured

`vs_proc_tests "[children]"`: a 3 × 2 grid of parametric cells on the
rack sample evaluates with every cell current, the composite's volume is
the cells' sum (24,000 mm³), and the rack, its base and the cells weld
into one closed, manifold mesh through the delegated seat and seven
generated joins; the same with procedural (shell and ball) cells. Output
hashes match on 1 and 4 threads. A second evaluation runs no generator
and regenerates nothing; moving the seat regenerates only the cell it is
passed to. Overlapping boxes fail the composite with `validate.overlap`, a
box outside it with `validate.nesting.outside`, over-deep fills with
`validate.containment` on each cell. Every existing test, sample and
golden hash is unchanged.

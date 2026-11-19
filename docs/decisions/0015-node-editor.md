# 0015: Node editor UI

Nov 19, 2026 · M15

## Questions

1. The plan offers QtNodes or a small custom canvas. Which?
2. Graphs are authored through `proc::edit()`, which refuses type errors
   and loops. How do canvas gestures become undoable edits?
3. Where do node positions live? The graph's JSON has none, and the M12
   templates' hashes must stay what they are.
4. How does a node preview reach the viewport fast enough ("within a
   second when its result is cached") while the volume may be
   regenerating?
5. How are fields drawn: the plan says "as an iso-surface or slice".
6. "Parameter editing in the inspector": the inspector is a read-only tree.
7. How do failed and infeasible nodes show on the canvas, and how are the
   inputs a message names found again?

## Decisions

**A small custom canvas on QGraphicsView.** QtNodes would be a new vcpkg
dependency (a cloud session can't add one, decision 0005) and brings its
own data model, which would sit beside `proc::Graph` and have to be kept in
step with it. Qt Widgets' graphics view already does items, selection,
rubber bands, zoom and drag and drop; the editor needs only node and edge
items on top. The rules live in `graph_edit.hpp` without Qt, as M14 did for
sketches.

**Every gesture is one `set_graph()` Command.** The canvas, palette and node
form call `graph_edit.hpp` functions (`add_node`, `connect_ports`,
`set_setting`, `expose_input`, ...), each a `proc::edit()` on a copy that
returns the new graph or the reason it was refused. The editor hands the
new graph to the main window as `set_graph(volume, graph, label)`, which the
History applies like any M14 edit: whole-project snapshots, so undo gives
the graph back exactly. `set_graph()` also runs `check_volume_graph()` and
refuses everything but "unfinished" (an input not connected yet, no
output), so a node reading a surface that doesn't join the volume is
refused when it is made. A change that changes nothing (a field left
without editing) makes no undo step. Wiring into a taken input replaces the
edge, since an input takes one edge; that keeps "drag a new wire over the
old one" a single gesture.

**Node positions aren't saved.** Putting them in the graph's JSON would
change every graph's bytes, the format version and the content hashes of
the M12 samples and templates, for something the evaluation never reads.
The editor keeps positions per node id while the app runs (undo and redo
included) and places nodes it hasn't seen in columns by depth, stacked by
their drawn height (`layered_layout()`); Auto layout redoes that. Saving a
layout beside the graph, outside its hash, can come later if people want
it.

**Previews run on their own thread with the generator's node cache.**
`Session::preview_node()` queues one request (the newest replaces any
waiting) for a thread that is not the evaluation worker, with its own small
thread pool, so a preview never waits behind a long generation. It calls
`proc::inspect_node()` with the procedural generator's `NodeCache`, so a
node the volume just generated through comes from the cache, and keeps the
volume's gathered inputs (its boundary and patches, tessellated) keyed by a
hash of the evaluated document without contents, so only the first preview
after a layout change pays for them. The graph previewed is the one the
editor shows (graph edits don't change the layout), against the latest
snapshot's layout build. The scene is made and prepared for OCCT on that
thread too; the UI thread only swaps the overlay in.

**Fields as a surface or a slice.** `proc::preview()` already meshes a
field's region on a coarse grid (32 cells along the box) and keeps the
samples. Surfaces draw that mesh; a slice draws one z level of the samples
as a grid coloured blue inside, white at the surface and red outside, with
the zero contour as lines, drawn unlit through per-vertex colours (new on
`DisplayMesh`). Frames draw a small diamond at each origin; meshes draw as
they are; numbers, vectors and lists are text on the editor's preview line.
The previewed volume's own contents are hidden while a preview shows, so
the two don't hide each other; selecting anything but a node ends the
preview.

**The inspector swaps in a node form.** For a selected node of the graph
being edited the inspector dock shows `NodeForm` in place of the property
tree: the node's name, a field per unconnected number or vector input (with
Expose, which makes it a graph parameter read by a new parameter node), and
a field per property (combo boxes for choices, the joining surfaces and the
graph's parameters). Its edits are emitted after the current event, because
the change they ask for rebuilds the form. The graph's exposed parameters
are a table in the Graph panel.

**Marks come from diagnostics' subjects.** A node is marked by any
diagnostic whose first subject is the node: the check, the last
evaluation's diagnostics for the volume, and the previewed node's
inspection. `proc.node.infeasible` names its inputs as further subjects
(ids derived from the node id and the port), which the editor maps back to
ports and rings. An input that must be connected and isn't is a warning
(amber), worked out from the graph rather than from
`proc.graph.missing_input`, whose subjects don't name the port. The
feature list says "failed" or "infeasible" beside the node too.

**Templates and baking are ordinary authoring.** The template library is
an `EditDialog`, so attaching is previewed live and committed with OK like
any M14 dialog (`make_procedural()`; a placeholder is filled, a procedural
volume's graph replaced). Bake takes the volume's generated mesh from the
latest full evaluation, and is refused until the volume has generated the
graph as it stands; unbake restores the kept graph. Both are one undo step.

## Results

On Dallas's desktop (`windows-msvc-app`):

- Each M12 template (`gyroid_infill`, `graded_lattice`, `bolt_bosses`) was
  built from scratch in the editor on the bolt_bosses sample's procedural
  volume, starting from the starter graph: parameters added in the
  parameters table, nodes added from the palette, names and settings typed
  into the node form, wires connected. Each built graph is the saved one
  but for node ids, and generates the same output hash (mesh, samples and
  grid) and the same published outputs as the saved template attached
  from the template library.
- Previewing every node of a 50-node graph (parameters, maths, vectors,
  lists, fields, lattices and TPMS, frames, Manifold mesh booleans,
  conversions, published outputs) after the volume generated: the slowest
  took 115 ms from selection to the overlay on screen (the first, which
  also gathers the volume's inputs); every cacheable node came from the
  node cache.

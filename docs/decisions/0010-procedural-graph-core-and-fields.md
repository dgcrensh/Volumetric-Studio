# 0010: The procedural graph core and fields

Nov 19, 2026 · M10

## Questions

1. Procedural volumes must "join the M9 scheduler", but the module rule
   puts `vs_eval` below `vs_proc`: the engine can't call the graph. How
   do they meet?
2. What is a field, and how is the boundary turned into one ("boundary as
   SDF, computed from the B-rep") without the kernel on worker threads?
3. Which checks happen when a graph is authored, and how can a graph be
   built one node at a time if an incomplete graph is an error?
4. Where do node settings live: ports, properties, or both? And what is a
   graph's "exposed parameter"?
5. What is cached per node, and by what key?
6. With no meshing until M11, what are a procedural volume's contents,
   and what about containment and published outputs?

## Decisions

**The engine takes generators.** `vs_eval` gains
`eval::ContentsGenerator` (`generator.hpp`) and
`Engine::set_generator(kind, g)`; `proc::install(engine)` registers the
procedural one. A generator `prepare()`s each regenerating volume on the
calling thread, before the wave starts, and returns a job the pool runs
(twice with `check_determinism`). Generated contents carry an opaque
`ExtraContents` (here the field and its samples) whose hash joins the
output hash only when present, so every M9 hash is unchanged. Without a
generator, procedural volumes stay Stale exactly as in M9. This keeps
the dependency arrow pointing down and lets later content kinds
(composite volumes) plug in the same way.

**Fields are lazy expression trees, evaluated in FP64, sampled to FP32.**
A `Field` is an immutable `FieldExpr` tree (primitives, booleans, smooth
booleans with the polynomial smooth minimum, offset, shell, maths) that
carries a digest of its whole tree, so equal fields built separately hash
equal. Nothing is computed until `sample()` evaluates the grid covering
the boundary's bounding box (points `cell` apart from its minimum corner,
x fastest; `cell` 0 gives 64 cells on the longest side; at most 2^24
samples). Each sample is computed alone and z slices run on the pool, so
thread count can't change a value. The smooth booleans and every
primitive use only arithmetic and `sqrt` (correctly rounded), never libm
transcendentals.

**The boundary is tessellated once, then it's plain arithmetic.**
`gather_inputs()` runs in `prepare()`, on the calling thread: it
tessellates the boundary with `vs_mesh::tessellate()` (closed, consistently
wound, 0.01 mm deflection) and each joining patch with the kernel. Signed
distance comes from a BVH over those triangles with angle-weighted
pseudonormals (Bærentzen and Aanæs), which gives the correct sign
everywhere, not just near the surface. For planar faces it is the exact
SDF (tests compare a box to its formula to 1e-9 mm); for curved faces it
is within the deflection (tests compare a cylinder to 0.02 mm and its sign
to the kernel's classifier). Jobs never touch the kernel.

**Authoring rejects what is wrong, not what is unfinished.**
`check_graph()` reports every problem (`proc.graph.*`). `edit()` and
`connect()` apply a change to a copy and keep it unless it has an error
other than `proc.graph.missing_input` or `proc.graph.output`, which only
say the graph can't run yet. So a type mismatch or a loop is refused the
moment it is made, while a graph can still be built node by node. Edges
join equal types only; there are no implicit conversions. `model::check()`
still treats graph JSON as opaque (vs_model is below vs_proc);
`check_volume_graph()` adds the encapsulation check (a node may read only
connection surfaces joining its volume, `proc.reference.surface`) and the
generator runs it before every generation.

**One settings map; parameters are declared by the graph.** A node's
`settings` hold literal values for its unconnected Scalar and Vector
inputs and its properties (numbers, vectors, choices, a surface id, a
parameter name). An edge into an input wins over a setting. The graph
declares its parameters with defaults; `input.parameter` reads the
volume's parameter of the same name, else the default. That way layout
variables and links drive graphs through the volume's existing
parameters, with nothing new in vs_model.

**The per-node cache keys on everything a node's outputs depend on**:
engine version, type, settings, derived seed and each input value's hash
(fields by tree digest). Nodes reading the graph inputs (`input.*`,
`query.*`) are cheap and never cached; their outputs' hashes carry the
inputs into every key below them. The generator owns one cache across
evaluations, so changing a parameter re-runs only the nodes downstream of
it (counted in tests). Each node gets a seed derived from the volume's
seed and its own id, so adding a node never changes another's randomness.

**Failures name one node.** A node reports infeasible inputs
(`proc.node.infeasible`: the node, then each input as
`<node>.<port>`, then the volume) or a failure (`proc.node.failed`);
evaluation stops there. Every proc diagnostic ends with the volume
subject, and routing (`vs_validate`) now takes a Node target's volume from
it; `proc.*` codes whose first subject is the volume route to its
contents.

**Contents are a sampled field for now.** `FieldContents` holds the field
and its samples; the output hash covers the grid and every sample. There
is no geometry, so the engine runs no containment check and published
outputs have no value (they arrive with meshing in M11 and measuring in
M12). Field contents stay in memory; the disk cache holds B-rep only. A
job, once started, runs to the end, as a kernel operation does.

## Consequences

- M11 meshes `FieldContents` (surface nets on the same grid) and adds the
  containment check for generated meshes; the grid rule above is what it
  will sample.
- New node families (M12) are `NodeType`s added to the library; the
  checks, cache and JSON need no change.
- `vstudio-cli nodes` lists the library, `graph <file>` checks a graph,
  and `rebuild` evaluates procedural volumes.

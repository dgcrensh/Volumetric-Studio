# Volumetric CAD Studio

A desktop CAD application in which every design is a tree of volumes. Read
these before writing code:

- `Volumetric CAD Studio — Product Spec.md`: what the product is and the seven design rules.
- `Implementation Plan.md`: the milestones (M0 to M17), the module layout and the conventions below.
- `docs/decisions/`: decisions that changed or refined the plan.

## Build and test

One command per platform configures, builds and runs every test:

```powershell
# Windows (Visual Studio 2022 with the C++ workload)
powershell -ExecutionPolicy Bypass -File scripts\build.ps1                 # windows-msvc-release
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Preset windows-msvc-debug
```

```bash
# Linux (git, cmake 3.25+, ninja, GCC 12+ or Clang 16+, curl, zip, unzip, tar, pkg-config,
# and OpenGL/X11 headers for OCCT: libgl1-mesa-dev libglu1-mesa-dev libegl1-mesa-dev
# libx11-dev libxext-dev libxi-dev libxmu-dev)
scripts/build.sh                    # linux-gcc-release
scripts/build.sh linux-clang-debug
```

Both scripts accept `--clean`/`-Clean` and `--skip-tests`/`-SkipTests`. They
use `VCPKG_ROOT` if it points at a git clone of vcpkg; otherwise they clone
vcpkg into a per-user cache (`%LOCALAPPDATA%\vstudio\vcpkg` or
`~/.cache/vstudio/vcpkg`) at the commit pinned by `builtin-baseline` in
`vcpkg.json`. The first build installs dependencies; Open CASCADE alone takes
well over an hour on a desktop. Later builds reuse vcpkg's binary cache.

After the first run, the presets work directly (from a VS developer shell on
Windows, with `VCPKG_ROOT` set):

```bash
cmake --preset windows-msvc-debug      # or linux-gcc-debug, linux-clang-release, ...
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug      # add -R <regex> or -L <module> to filter
build/windows-msvc-debug/apps/vstudio-cli/vstudio-cli selftest
```

Presets: `windows-msvc-{debug,release,app}`, `linux-{gcc,clang}-{debug,release}`.
Release presets are `RelWithDebInfo`. Build trees go to `build/<preset>/`.

### Options and vcpkg features

| CMake option | Default | Effect |
| --- | --- | --- |
| `VS_BUILD_APP` | OFF | Builds the Qt app (`vs_app`, `vs_app_tests`) and turns on the vcpkg `app` feature (Qt 6). Only the `windows-msvc-app` preset sets it. |
| `VS_BUILD_TESTS` | ON | Builds the Catch2 suites and registers them with CTest. |
| `VS_WARNINGS_AS_ERRORS` | ON | `/WX` or `-Werror` on first-party code. |

Heavy dependencies sit behind vcpkg manifest features so that each milestone
installs only what it uses (see `docs/decisions/0000-optional-vcpkg-features.md`):
`kernel` (OCCT, from M1), `sketch` (Eigen, from M2), `proc` (Manifold, from
M12) and `app` (Qt). A milestone that starts using one appends it to
`VCPKG_MANIFEST_FEATURES` in the top-level `CMakeLists.txt`, before `project()`.
`kernel` is on in every build since M1, `sketch` since M2 and `proc` since M12.

### Prebuilt dependencies outside CI (cloud sessions)

CI's vcpkg cache lives in `actions/cache`, which only Actions jobs can read.
So the `linux-gcc-release` job on `main` (and manual runs) also publishes its
x64-linux binary cache as assets of the `vcpkg-cache-x64-linux` prerelease
(see `docs/decisions/0004-shared-vcpkg-binary-cache.md`). On a fresh Linux
machine, such as a hosted cloud session, run this before building:

```bash
scripts/cloud-setup.sh              # apt packages, fetch the cache, build linux-gcc-release
scripts/cloud-setup.sh --no-build   # everything but the build
scripts/vcpkg-cache.sh fetch        # only refresh the cache (uses GH_TOKEN or GITHUB_TOKEN if set)
```

The cache only hits with the same compiler as CI (Ubuntu 24.04, GCC 13.3);
anywhere else vcpkg falls back to building OCCT from source.

A cloud session can't download vcpkg port sources (GitHub's archive
downloads of other repositories are refused), so a **new vcpkg dependency
can't be added from a cloud session**: land it from a desktop or let CI on
`main` build and publish it first (decision 0005). Release assets do
download, so a small library can be vendored under `third_party/` instead,
as miniz was (decision 0006).

## Layout

```
cmake/                  build helpers: compile options, module rule, vs_add_library
libs/<module>/          one directory per library
  include/vs/<short>/   public headers (namespace vs::<short>)
  src/                  sources and private headers
  tests/                Catch2 tests, built as <module>_tests
third_party/<name>/     vendored code with its own CMake target (PlaneGCS, miniz); see its README
apps/vstudio-cli/       command-line driver (subcommands grow with each milestone)
apps/vs_app/            Qt application (VS_BUILD_APP=ON only)
tests/cmake/            tests of the build system itself
scripts/                one-command build and test scripts
docs/decisions/         one short Markdown file per decision
```

## Module dependency rule

Libraries, bottom up. A library may link only libraries listed before it.
Applications may link any library, but never each other, and nothing links an
application.

```
vs_core → vs_kernel → vs_sketch → vs_feature → vs_model → vs_validate → vs_mesh → vs_eval → vs_proc
apps: vs_app, vstudio-cli
```

This is enforced at configure time: `vs_verify_module_graph()` (in
`cmake/VsModuleRules.cmake`) walks every registered target and fails the
configure on a forbidden link. Create targets with `vs_add_library`,
`vs_add_library_tests` and `vs_add_application` from `cmake/VsModules.cmake`
so they are registered. A new library must also be added to
`VS_LIBRARY_ORDER`.

## Conventions (from the plan)

1. Read the plan, the spec and this file before writing code.
2. One branch per milestone, named `m<N>-<slug>`, and one PR against `main`.
3. Keep `main` building on Windows (MSVC) and, for everything except
   `vs_app`, on Linux (GCC and Clang). CI checks both.
4. Add tests for every acceptance item of the milestone. It isn't done until they pass.
5. Record any decision that changes the plan in `docs/decisions/NNNN-<slug>.md`
   and update the plan's status table.
6. Keep OCCT, PlaneGCS and Manifold types out of every public header except
   their own wrapper module (`vs_kernel`, `vs_sketch`, `vs_proc`).

## vs_core essentials

- **Precision split.** Authored geometry uses FP64 (`dvec3`, `dmat4`,
  `dtransform`, `dbox3`); generated meshes, fields and display use FP32
  (`fvec3`, `fmat4`, `ftransform`, `fbox3`). There are no implicit
  conversions; use `to_fp32()` / `to_fp64()`.
- **Units.** Lengths are stored in millimetres, angles in radians. Convert at
  the edges with `to_mm()` / `from_mm()` and `degrees()`.
- **Settings.** `ProductSettings` holds the working space (±1,000 mm) and the
  linear tolerance (1e-6 mm). Pass it explicitly; don't hard-code tolerances.
- **IDs.** `Id` is 128-bit. `Id::random()` for authored objects;
  `Id::derive(parent, name_or_index)` for anything a procedure generates, so
  regeneration is deterministic.
- **Hashing.** `Hasher` / `hash_of(...)` (BLAKE3) with an explicit,
  platform-independent encoding (integers widened to 64-bit little endian,
  floats canonicalised). Give each hashable type a `hash_append(Hasher&, const
  T&)` overload next to it. The golden values in
  `libs/vs_core/tests/test_hash_golden.cpp` must never change without a
  deliberate decision and a bump of `kEngineVersion`.
- **Diagnostics.** Report every user-facing problem as a `Diagnostic` with a
  stable dotted `code` owned by the module (for example `validate.overlap`)
  and its subjects; the first subject is the "act here" target. Return
  `Result<T>` instead of throwing across module boundaries. Tests match on
  codes, never on message text.
- **Logging.** `vs::core::logger()` (spdlog) is for developers only.
- **Determinism.** Floating-point contraction (FMA) is off on every compiler.
  Don't hash values that went through libm (`sin`, `cos`, ...) in golden tests;
  they may differ in the last bit between platforms.

## vs_kernel essentials

- **Handles, not OCCT types.** `Shape`, `Solid`, `Shell`, `Face` and `Edge` are
  cheap, immutable, reference-counted handles. `Solid` may hold several lumps
  (`solid_count()`). OCCT is linked privately; ctest compiles every public
  header without it and scans them for `.hxx` includes.
- **Never throws.** Every operation returns `Result<T>` or `Diagnostics` with a
  `kernel.*` code. Results of booleans, transforms and fillets have already
  passed `check_validity`.
- **Settings in, not constants.** Builders take `ProductSettings` and reject
  sizes under the minimum feature size and shapes outside the working space.
  `audit_tolerances()` compares OCCT's per-entity tolerances with the
  product tolerance; call it after each feature (M3).
- **Sweeps, lofts, shells, chamfers** (M4) live beside the primitives and
  fillets. Booleans also take several tools at once. `shell()` rejects
  walls thicker than a curved face's radius up front, because OCCT returns
  a valid-looking inside-out solid for them.
- **Exchange.** STEP (AP242, mm) and IGES go through `exchange.hpp`; calls
  are serialised because OCCT's exchange layer has global state.
  `write_brep()` / `read_brep()` (OCCT's own text, no triangulations) and
  `shape_hash()` serve M9's cache keys and disk cache.
- **Tessellation** (`tessellate.hpp`, M8) runs BRepMesh on a copy and
  returns plain data: vertices, each edge's division, each face's wires
  and triangulation with where every point sits (inside, vertex, k-th edge
  point). It knows nothing of seams; `vs_mesh::tessellate()` builds on it.
- **Tolerance findings** are in `docs/decisions/0001-working-space-and-tolerance.md`;
  `vstudio-cli tolerance-spike` reruns the measurements.

## vs_sketch essentials

- **Model.** A `Sketch` holds entities (points, lines, circles, arcs) and
  constraints, each with a stable `Id`. Curves refer to point entities, so
  sharing a point joins two curves. Arcs run counter-clockwise from start to
  end. Dimensions carry a `value` and an optional `expression` (evaluated by
  M3; until then only `value` is used). `validate()` reports broken
  references and malformed constraints (`sketch.reference.*`,
  `sketch.constraint.*`).
- **Solver.** `SketchSolver` (`make_solver()`, or the free `solve()` and
  `drag()`) wraps PlaneGCS, vendored in `third_party/planegcs` and linked
  privately; ctest checks that no public header includes it or Eigen. A
  solve reports `Solved`, `UnderConstrained` (dof and free entities),
  `OverConstrained` (conflicting and redundant constraints by id) or
  `Failed`, also as `sketch.solve.*` diagnostics. Conflicts leave the sketch
  untouched; redundancy alone still solves.
- **Profiles.** `extract_profiles()` finds closed loops of non-construction
  geometry, nests them (even depth: outer, odd: hole) and `make_face()`
  turns them into `vs_kernel` faces on the sketch plane.
- **Files.** `to_json()` / `sketch_from_json()` round-trip exactly, ids
  included. `vstudio-cli sketch-solve <file>` solves one from the command line.

## vs_feature essentials

- **Tree.** `FeatureTree` holds a `VariableTable` and ordered `Feature`s
  (sketch, datum plane/axis/point, extrude, revolve, and from M4 sweep,
  loft, shell, fillet, chamfer, linear and circular pattern), each with a stable
  `Id`. Features refer to each other by id through `PlaneRef`, `AxisRef`,
  `PointRef` and `FaceRef`; numbers users can drive are `Param`s (a value
  plus an optional expression).
- **Variables.** `expression.hpp` is the expression language (arithmetic,
  functions, units that convert into mm and radians). `VariableTable::add()`
  and `set_expression()` reject loops when authored, with
  `feature.variable.loop` naming the loop as a path.
- **Rebuild.** `rebuild()` runs top to bottom on the calling thread. A
  failed feature leaves the body unchanged and the rest carries on; its
  diagnostics (`feature.*`, or `kernel.*` passed through) name the feature
  first. Pass a previous result and a start feature to reuse what's above
  it. Every feature boolean is fuzzy at the linear tolerance, and the
  tolerance audit runs after each body change (decision 0002).
- **Face references** name a face by feature and name (`start`, `end`,
  `side:<sketch curve id>`, `<k>:<seed id>:<name>` on pattern copies) and
  are found again by plane or cylinder; see `rebuild.hpp`. **Edge
  references** are one or two face references: the edges where they meet,
  or every edge of one (decision 0003).
- **M4 features.** Sweep (path = a whole sketch, one chain), loft (one
  holeless profile per section sketch), shell, fillet, chamfer, linear and
  circular patterns of extrudes, revolves, sweeps and lofts. A failed
  fillet, chamfer or shell searches for a value that works and reports it
  as `feature.fallback.works`; the body is never left invalid.
- **Files.** `to_json()` / `feature_tree_from_json()` round-trip exactly.
  `vstudio-cli build <file> [out.step]` rebuilds one and writes STEP.

## vs_model essentials

- **Document.** `Document` holds the layout (a `FeatureTree`), the volumes
  under it (one level), and the connection surfaces, keep-outs and links the
  root owns. Ids are unique across the whole document, contents included.
  A `Volume` has a boundary (layout solid features' tools, or a box between
  two layout points), a keep-out flag, contents (static file, parametric
  tree, procedural graph JSON), parameters, a seed and published outputs.
  Static contents keep `baked_graph` when a procedural volume was baked
  (M12); `OutputQuantity::Published` takes a number the graph publishes.
- **Authoring.** Change a document through `edit(doc, fn)` (`check.hpp`): it
  applies `fn` to a copy and keeps it only if `check()` finds no errors.
  `check()` covers structure (`model.*`), encapsulation
  (`model.reference.contents` / `.sibling` / `.not_input` / `.parent`, all
  judged by `resolve()` in `references.hpp`) and dependency loops
  (`model.dependency.loop`, path with the link first, `dependency.hpp`).
- **Contents see inputs, nothing else.** A parametric tree is rebuilt with
  `feature::ExternalInputs`: the boundary by the volume's id (faces
  `<layout feature id>:<name>`, or `-x`..`+z` for a box), each joining
  connection surface by its id (a plane, face `patch`), and parameters as
  values. Published outputs reach anything only through a `Link` to a layout
  variable or parameter.
- **Build.** `build(doc, settings, &blobs)` rebuilds the layout, boundaries
  (keep-outs cut), patches (with bolt hole centres) and contents, and
  measures outputs. Status (`VolumeStatus`) is on the result, never saved.
  It is `build_layout()` (layout, boundaries, patches, parameters) then
  `build_contents()` per volume, which only reads the layout and may run
  on several threads; vs_eval calls the halves itself.
- **Files.** `.vsproj` is a folder or a zip of `project.json` plus
  `blobs/<hash>.<ext>` (`project.hpp`, decision 0005). Zips go through the
  vendored miniz (deflated, zero timestamps, decision 0006). Loading checks
  blob hashes and runs `check()` (`load_project(path, false)` skips it, for
  validation). `content_hash()` hashes the document's
  JSON plus the blobs. `vstudio-cli info <project> [--build]` prints one.
  `.gitattributes` keeps `*.vsproj/**` byte-exact; regenerate the sample
  with `vs_model_tests "[.generate]"`.

## vs_validate essentials

- **Layout checks** (`layout.hpp`): `validate_layout(doc, build, settings)`
  takes a `model::build()` result. Overlap (`validate.overlap`: kernel
  `common`, depth = the region box's thinnest side, deeper than the
  tolerance), undeclared contact (`validate.contact` per pair of faces
  lying against each other, named from `VolumeBuild::boundary_faces`;
  `validate.contact.edge` for an edge or point touch), skipped for pairs
  sharing a connection surface or involving a keep-out. Connection rules
  (`validate.connection.*`): patch on each joined boundary, clearance gap
  and flange wall at least the minimum feature size, ISO 273 holes clear of
  the patch edge and each other by the minimum wall.
- **Containment** (`containment.hpp`): `check_containment(boundary, solid
  or MeshView, volume subject, settings)`, for M9 to call after generation.
  Meshes get their FP32 rounding as extra slack. Large meshes (M12): a
  convex fast path (one lump, planar faces), otherwise octree cells
  certified by distance and the rest classified in chunks through an
  optional parallel-for.
- **Routing** (`registry.hpp`): every `validate.*` code is registered with
  its severity and target; `route()` sends any diagnostic to exactly one of
  layout, connection surface, feature (with the volume, inside contents),
  node or contents. Tests check every emitted `validate.*` code is
  registered.
- **Whole documents** (`validate.hpp`): `validate()` runs `check()`, then
  `build()`, the layout checks and containment, and routes everything.
  `vstudio-cli validate <project>` prints it and exits 1 on errors.
- **Fixtures** for each check live in `libs/vs_validate/tests/data`;
  regenerate them with `vs_validate_tests "[.generate]"`. Decision 0006 has
  the reasoning.

## vs_mesh essentials

- **Meshes** (`mesh.hpp`): FP32 positions, indexed triangles (counter-clockwise
  from outside), a volume id per triangle and a seam vertex id per vertex
  (null off seams). `analyse()` (`topology.hpp`) reports boundary,
  non-manifold and inconsistent edges, non-manifold vertices and shells;
  `HalfEdges` gives twins. `view()` hands a mesh to `check_containment()`.
  `analyse()` is linear (half-edges bucketed by vertex), for meshes of
  millions of triangles.
- **Seams** (`seam.hpp`): one per bonded or bolted-flange connection surface
  (clearance surfaces get none), made once by `build_seams()`
  (`seam_library.hpp`) from the built patch's exact outline and the
  surface's spacing (`seam.spacing`, else a sixteenth of the diagonal).
  Flange bolt holes are hole loops. Outline segments divide into
  `ceil(length / spacing)` pieces (arcs at least 16 a turn); the interior is
  a lattice, constrained-Delaunay triangulated (`triangulate.hpp`, our own).
  Vertex ids derive from a hash of the inputs. Meshers never divide a seam
  themselves: they call `add_seam_vertices()` and `add_seam_face()`, and
  join their own faces to `Seam::loops` (with `segment_starts`).
- **Weld** (`weld.hpp`): merges vertices by seam id only, checks each volume
  has exactly the seam's triangles (`mesh.weld.nonconforming`, surface
  first), removes the paired seam faces (and paired `Seam::hole_fills`,
  where neither side has a bolt hole) and checks the result is watertight
  and manifold. Decision 0007 has the reasoning.
- **B-rep tessellation** (`tessellate.hpp`, M8): BRepMesh's mesh, except
  that vertices on a seam's outline become seam vertices (snapping to the
  nearest when between two), edges along it take the seam's division,
  planar faces on it carry the seam's triangles (constrained Delaunay
  round the patch, `triangulate()` with extra segments) and faces whose
  edges changed are re-triangulated in (u, v) with BRepMesh's interior
  points. A face covering only part of a patch is
  `mesh.tessellate.seam_uncovered`. Decision 0008.
- **Combined mesh** (`combine.hpp`): tessellate every non-keep-out volume
  (B-rep contents; mesh blobs read as they are; placeholders only with
  `placeholders`), weld. Static mesh blobs get their seam vertices back by
  exact FP32 position (`attach_seam_vertices()`), so baked volumes weld.
  **Files** (`io.hpp`): STL out (binary) and in, 3MF out (one object, or
  one per volume with its id) and in (our own writer and XML reader,
  zipped with miniz), OBJ in. `import.hpp` adds a
  mesh file as a static volume boxed by its bounds.
- **Tests** mesh volumes with small meshers in `libs/vs_mesh/tests/meshers.*`
  (prisms, pyramids, a box round a patch) and weld random outlines;
  `test_tessellate.cpp` welds B-rep pairs (cylinders, cones, domes, fillets,
  bolt holes) in random poses. `vstudio-cli seams <project>` prints a
  project's seams; `export <project> --stl|--3mf [--per-volume]` and
  `import <project> <mesh>` write and read mesh files.

## vs_eval essentials

- **Engine** (`engine.hpp`): `Engine(settings, {threads, check_determinism,
  disk_cache})`, then `evaluate(project, {generate, weld, cancel,
  progress})`: `check()`, `build_layout()` and `validate_layout()` on the
  calling thread, keys, cache, changed volumes generated in parallel,
  containment (on the calling thread), outputs published through links,
  optional weld. Volumes downstream of others through links run in later
  waves, the layout rebuilt with the link values. `preview()` is the same
  without generating. The engine keeps each volume's last valid contents.
- **States.** Current; Failed (generation or containment failed, or the
  boundary or a parameter: shows its last valid contents as
  `stale_contents`, blocks only its dependants, which get `eval.blocked`);
  Stale (blocked, cancelled, preview, procedural with no generator); Regenerating
  while running (`states()`). Failed results are cached too.
- **Cache** (`cache.hpp`): `cache_key()` hashes engine version, kind,
  content definition, the boundary's exact shape and named faces, joining
  surfaces, parameters, seed and measured outputs; static keys leave
  parameters and seed out. Memory always; disk (`cache/<key>.brep` and
  `.json`) for clean B-rep contents only. `measure.hpp` measures outputs
  of meshes too (public since M12, for vs_proc). `vstudio-cli rebuild
  --cache` uses a folder project's `cache/`.
- **Determinism** (`thread_pool.hpp`): `run()`, `parallel_for()` and
  `parallel_reduce()` over fixed-size chunks, folded in chunk order; nested
  calls share the workers (a stack of batches; callers work on their own
  while waiting), so one volume's generation uses every thread. Catch2
  assertions aren't thread-safe: tasks record, tests check afterwards.
  `output_hashes_by_threads()` and `check_determinism` compare output hashes (BRep text, mesh, outputs).
- **Generators** (`generator.hpp`): a library above vs_eval plugs a
  content kind in with `Engine::set_generator(kind, g)`: `prepare()` on
  the calling thread, then a job on the pool that fills
  `GeneratedContents` (`extra` holds opaque contents, hashed into the
  output hash). vs_proc's is installed by `proc::install()` (M10).
- **Imports** (`import.hpp`): STEP, IGES, STL, 3MF, OBJ as a new static
  volume boxed by its bounds, or `import_contents()` into an existing one
  (then containment matters). `eval.*` codes on a volume route to its
  contents. Decision 0009.
- **Tests** build scenes in code (`tests/scenes.*`: a row of parametric
  boxes; a chain A → B by a link, plus C) and reuse the bracket.
  `vstudio-cli rebuild <project> [--threads N] [--check-determinism]
  [--weld] [--cache] [--set vol.param=value] [--twice]`.

## vs_proc essentials

- **Types** (`types.hpp`): ports are Scalar, Vector (FP64), Field, Mesh,
  Frames, Curves (FP32) or List; a `Value` holds one, immutable and cheap
  to copy, with a content hash (`hash_value()`). Edges join equal types
  only; conversions are nodes.
- **Fields** (`field.hpp`): lazy `FieldExpr` trees (primitives, booleans,
  smooth booleans, offset, shell, maths), evaluated in FP64 with no libm
  transcendentals, each carrying a digest of its tree. `grid_covering()`
  and `sample()` give FP32 samples on a grid over a box (x fastest),
  z slices on the pool. `TriangleDistance` (`distance.hpp`) is a BVH over
  a tessellation: signed by angle-weighted pseudonormals for closed
  meshes, unsigned for patches.
- **Graphs** (`graph.hpp`): parameters with defaults, nodes (id, type,
  name, `settings` for unconnected Scalar/Vector inputs and properties),
  edges output → input, exactly one `output` node. JSON round-trips
  exactly. **Checks** (`check.hpp`): `check_graph()` (`proc.graph.*`);
  `edit()` / `connect()` keep a change unless it is wrong (type mismatch,
  loop, ...), allowing unfinished graphs; `check_volume_graph()` adds
  `proc.reference.surface` (only surfaces joining the volume).
- **Nodes** (`nodes.hpp`): a `NodeType` declares ports and properties and
  evaluates from a `NodeContext` (inputs, properties, graph inputs,
  derived seed); `infeasible(ports, msg)` / `fail(msg)` report
  `proc.node.*` naming the node, its inputs and the volume. `library()`
  has input, query, field, math, logic, vector, list and convert nodes; `vstudio-cli
  nodes` lists them.
- **Inputs and evaluation** (`inputs.hpp`, `evaluate.hpp`):
  `gather_inputs()` tessellates the boundary and joining patches on the
  calling thread; `evaluate_graph()` runs nodes in topological order with
  a `NodeCache` keyed by type, settings, seed and input hashes (input and
  query nodes always run).
- **Engine** (`generator.hpp`): `proc::install(engine)` registers the
  procedural `eval::ContentsGenerator`; contents are a mesh
  (`GeneratedContents::mesh`, `contents_mesh()`), which the engine checks
  for containment and welds as it is, plus `FieldContents` (field,
  samples, stitched surfaces; `field_contents()`). Outputs have no value
  until M12. Decision 0010.
- **Meshing** (`mesher.hpp`, M11): `surface_nets()` is surface nets on
  the grid's tetrahedra (always closed and manifold), merged per cube by
  link-condition edge collapses. `mesh_contents()` clips the field
  `kContentsInset` inside the boundary, meshes it, replaces each joining
  seam's patch with the seam's triangles stitched to the rim (or warns
  `proc.mesh.seam_uncovered`), then moves vertices inward until every
  vertex and centroid is contained. `convert.field_to_mesh` (with its
  `samples` cost) and `convert.mesh_to_field` convert inside graphs.
  `mesh::combine()` takes generated meshes from the engine. Decision 0011.
- **Node library** (M12, decision 0012): `field.lattice` (cubic, BCC,
  octet struts) and `field.tpms` (gyroid, Schwarz P, diamond) with cell
  and thickness, gradable by a field; `field.thicken`, `.clamp`,
  `.instances` (a template field at frames); frames (`frames.*`,
  `scatter.volume` / `.surface`, seeded and uncached, `pattern.linear` /
  `.circular` / `.grid`); meshes (`mesh.box`, `.cylinder`, `.instances`,
  `.measure`, and `.union` / `.difference` / `.intersection` through
  Manifold, linked privately, output in canonical order);
  `output.publish`. Fields use `sin_det()` / `cos_det()`, never libm.
  Contents are always a field: meshes reach them by `convert.mesh_to_field`.
  Shared node helpers are in `src/node_helpers.hpp`.
- **Outputs**: the generator measures the mesh (`eval::measure_mesh()`)
  and fills `Published` outputs from `output.publish` nodes
  (`proc.output.unpublished` when missing).
- **Inspection** (`inspect.hpp`): `inspect_node()` / `inspect_volume_node()`
  evaluate one node and its ancestors (`EvaluateGraphOptions::only_for`),
  so unfinished graphs inspect; `preview()` makes a coarse mesh, points
  or text of any value.
- **Bake** (`bake.hpp`): `bake()` stores the generated mesh as a 3MF blob
  and keeps the graph; `unbake()` restores it, same content hash.
- **Templates** (`templates.hpp`): `gyroid_infill`, `graded_lattice`,
  `bolt_bosses` (needs a bolted flange); ids derive from the name, so a
  template is the same graph every time. Saved copies in
  `tests/data/templates`.
- **Tests** build box rows in code (`tests/scenes.*`, and `mixed()`: a
  procedural box bonded or bolted to a parametric one); sample files in
  `tests/data` (regenerate with `vs_proc_tests "[.generate]"`, then
  restore any sample whose ids changed and that you didn't mean to);
  `vstudio-cli graph <file>` checks one; `rebuild --stl <out>` writes the
  welded mesh; `templates`, `inspect <project> <volume> <node>
  [--preview]`, `bake` / `unbake <project> <volume> [--out <path>]` and
  `bench-gyroid [--size mm] [--cell mm] [--threads N] [--3mf out]` (the
  CPU baseline) cover M12.

## vs_app essentials

- **Build** with the `windows-msvc-app` preset only (Qt 6 from the vcpkg
  `app` feature; its first build compiles qtbase twice, for the host tools
  and the target). CI doesn't build the app. Everything but `main.cpp` is
  `vs_app_lib`; `vs_app_tests` drives it. Tests tagged `[gui]` open
  windows and render with OpenGL, so they need a desktop session and a GPU.
- **Layers** (decision 0013): `scene.*` turns an `eval::Evaluation` into
  display meshes (B-reps via `kernel::tessellate()`, meshes as they are),
  `outline.*` is the panels' data (selection, rows, inspector, diagnostic
  routing via `validate::route()`), `exporter.*` is STL/3MF export, all
  without Qt. `session.*` runs the engine on one worker thread and delivers
  a preview snapshot, then the full one, on the UI thread. `viewport.*` is
  OCCT's AIS/V3d in a native `QWidget` (OCCT's visualisation toolkits
  only; no OCCT in any header). `main_window.*` lays out the docks.
- **Authoring** (M14, decision 0014): every edit is a `Command`
  (`authoring.hpp`) applied through `History` (`history.hpp`), which keeps
  whole project snapshots, so undo restores identical hashes and refuses
  changes that add a `model::check()` error. `reference_choices.hpp` lists
  what a feature may refer to (inside a volume: only its boundary faces,
  connection surfaces and own features). `sketch_editor.*` is sketch mode's
  rules (no Qt); `sketch_mode.*` its canvas and panel, replacing the
  viewport until Finish. `feature_dialog.*` and `model_dialogs.*` are
  modeless `EditDialog`s (`widgets.hpp`): `changed()` previews through
  `History::try_apply()`, OK commits. `MainWindow::edit_in_context()` dims
  every other volume. Tests drive these entry points; the bracket is
  rebuilt from scratch in `test_authoring_ui.cpp`.
- **Node editor** (M15, decision 0015): `graph_edit.*` is the rules (no
  Qt): every canvas gesture is a `proc::edit()` returning the new graph,
  applied as one `set_graph()` Command (which also refuses what
  `check_volume_graph()` refuses beyond "unfinished"). `node_editor.*` is
  the Graph dock (palette, QGraphicsView canvas, exposed parameters),
  `NodeForm` (a selected node's settings, swapped into the inspector dock;
  its name field is `".name"`) and `TemplateDialog`. Node positions are
  per session, not in the graph. `Session::preview_node()` inspects on its
  own thread with the generator's `NodeCache` and cached `GraphInputs`;
  `node_preview.*` turns `proc::preview()` into overlay items
  (`ItemRole::NodePreview`, `Viewport::set_overlay()`; the volume's
  contents hide meanwhile). `MainWindow::edit_graph()`,
  `attach_template()`, `bake()`, `unbake()`, `preview_node()` are the
  entry points tests drive.
- **Running it**: `build/windows-msvc-app/apps/vs_app/vs_app.exe [project]
  [--select <name>] [--view iso|top|front|...] [--context <volume>]
  [--sketch <sketch>] [--edit <feature>] [--graph <volume> [--node <node>]
  [--slice]] [--size WxH]
  [--screenshot out.bmp] [--viewport-image out.bmp]`; the capture flags
  wait for the evaluation, save (BMP: this Qt has no PNG) and quit.
- **From a Claude desktop-app session** `%LOCALAPPDATA%` is virtualised and
  Qt's configure mistakes it for a symlink: configure once with
  `-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=C:/vsbt` (decision 0013).

## Code style

- C++20, warnings as errors. `#pragma once`. Namespaces `vs::<short>`.
- Types `PascalCase`, functions and variables `snake_case`, constants `kPascalCase`.
- Public headers document intent and invariants; keep implementation detail in `src/`.

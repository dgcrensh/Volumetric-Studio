# Volumetric CAD Studio — Implementation Plan

Nov 18, 2026 · companion to *Volumetric CAD Studio — Product Spec.md*

This plan turns the MVP in the spec into 18 milestones (M0 to M17). Each one is sized for a single Opus 5.5 session: one focused subsystem, roughly 2–5k lines of code and tests, ending in a pull request that builds and passes its tests. After the MVP comes a short list of later milestones, kept deliberately rough.

## Decisions this plan rests on

| Area | Decision |
| --- | --- |
| Language and build | C++20, CMake (presets), vcpkg manifest mode |
| UI | Qt 6 (Widgets), OCCT's AIS/V3d viewer embedded in a Qt widget |
| Kernel | Open CASCADE (OCCT) behind a small internal kernel interface; OCCT types never leave `vs_kernel` |
| Mesh booleans inside procedures | Manifold (Apache 2.0) |
| Sketch solver | **Decided (Nov 18, 2026):** PlaneGCS, extracted from FreeCAD (see "Choosing a sketch solver" below) |
| Tests | Catch2 v3; every library builds and tests headless |
| Procedural evaluation | CPU only for the MVP, multithreaded and deterministic. GPU evaluation is a later milestone |
| Where work runs | Dallas's Windows desktop for now. Everything except the app (`vs_app`) must also build and test headless on Linux, so cloud sessions stay possible. A Windows GPU VM on Compute Engine is scripted, with the desktop's prebuilt dependencies (decision 0019, `docs/dev-vm.md`) |
| Repository | A new GitHub repo (set up by Dallas before M0). One branch and one PR per milestone |
| Working space and tolerance | ±1,000 mm box, 1e-6 mm linear tolerance, held as product settings. M1 tests whether OCCT copes |
| Connection surfaces (MVP) | Planar only, with seams derived automatically from the surface outline. Types: bonded, clearance, bolted flange |
| Field-to-mesh | One algorithm: surface nets on a regular grid, constrained to seams (M11). Dual contouring can replace it later behind the same node |
| Scripting / user-defined nodes | Not in the MVP (already decided in the spec) |
| Runnable output | Until the GUI milestones, each milestone is exercised through tests and the `vstudio-cli` tool (build a project file, validate it, export meshes) |

## Choosing a sketch solver

Writing a robust 2D geometric constraint solver is a project in its own right, so the plan uses an existing one. Licensing doesn't matter here, because the studio is never distributed.

| Option | What it is | For | Against |
| --- | --- | --- | --- |
| **PlaneGCS** (FreeCAD) | The C++ solver behind FreeCAD's Sketcher, built on Eigen. LGPL | Mature, purpose-built for CAD sketches. Lines, arcs, circles, ellipses, B-splines. Reports degrees of freedom, conflicting and redundant constraints (needed for the spec's "one place to act" diagnostics). Supports dragging, for interactive editing | Not packaged as a library. It has to be vendored from FreeCAD's source (`src/Mod/Sketcher/App/planegcs`) with a thin CMake wrapper. Light documentation |
| **SolveSpace `libslvs`** | SolveSpace's solver exposed as a small C library. GPL v3 | Clean C API, easy to build. Can also solve 3D constraints | Fewer sketch-oriented features (B-splines, fine-grained redundancy reports). The API is lower level, so we'd write more glue |
| Write our own | Newton / Levenberg–Marquardt over our own constraint set | Full control | Months of edge cases (convergence, redundancy, dragging). Poor use of time |
| Commercial (Siemens D-Cubed 2D DCM) | Industry standard | Best in class | Commercial licence only, same problem as Parasolid |

**Decision: PlaneGCS** (confirmed by Dallas, Nov 18, 2026). It already does what the authoring layer needs, especially DOF and conflict reporting. M2 wraps it behind our own `SketchSolver` interface, so swapping to `libslvs` later is contained to one module.

## Architecture at a glance

Libraries, from the bottom up. A library may depend only on those above it in this list.

| Module | Responsibility | First milestone |
| --- | --- | --- |
| `vs_core` | Math (FP64 and FP32 vectors, transforms, boxes), units, tolerance settings, IDs, deterministic hashing (BLAKE3), diagnostics types, logging | M0 |
| `vs_kernel` | The kernel interface and its OCCT implementation: solids, faces, edges, primitives, booleans, validity and tolerance checks, distance queries, STEP/IGES I/O | M1 |
| `vs_sketch` | Sketch entities, constraints, PlaneGCS wrapper, profile (closed loop) extraction | M2 |
| `vs_feature` | Feature tree, reference geometry, variables and equations, rebuild | M3, M4 |
| `vs_model` | Document, volume tree, boundaries, keep-outs, connection surfaces, published outputs, reference resolution, dependency graph, project file format | M5 |
| `vs_validate` | Layout checks (overlap, undeclared contact, type mismatch, loops), containment, diagnostics routing | M6 |
| `vs_mesh` | Mesh types, seam library, conforming tessellation, welding, mesh I/O (STL, 3MF, OBJ) | M7, M8 |
| `vs_eval` | Scheduler, cache, thread pool, cancellation, volume states | M9 |
| `vs_proc` | Node graph, type system, node library, fields, field-to-mesh, baking | M10–M12 |
| `vs_app` | Qt application: viewport, trees, inspector, sketcher, node editor | M13–M16 |
| `vstudio-cli` | Command-line driver over the libraries, used for tests and scripting during development | M0 onward |

Two rules from the spec are enforced by structure, not discipline:

- **Encapsulation** lives in `vs_model`'s reference resolver: the only handles that cross a volume boundary are `BoundaryRef`, `ConnectionSurfaceRef` and `PublishedOutputRef`. There is no API that returns a reference into contents.
- **Precision split** lives in the types: authored geometry uses `dvec3`/`dmat4` (FP64); generated meshes and fields use `fvec3` (FP32). Conversions are explicit functions.

## Conventions for every milestone session

Each session should:

1. Read this plan, the spec and the repo's `CLAUDE.md` before writing code.
2. Work on a branch named `m<N>-<slug>` and open one PR against `main`.
3. Leave `main` building on Windows (MSVC) and, for every library outside `vs_app`, on Linux (GCC or Clang).
4. Add tests for every acceptance item listed in its milestone. A milestone isn't done until those pass.
5. Record any decision that changes this plan in `docs/decisions/` (one short Markdown file per decision) and update this plan's status table.
6. Keep OCCT, PlaneGCS and Manifold types out of every public header except their own wrapper module.

## Milestone overview

| # | Milestone | Depends on | Main output |
| --- | --- | --- | --- |
| M0 | Foundations and build | none | Repo skeleton, `vs_core`, CLI stub |
| M1 | Kernel interface over OCCT | M0 | `vs_kernel`, tolerance spike result |
| M2 | Sketches and constraint solving | M0 | `vs_sketch` with PlaneGCS |
| M3 | Feature tree, core features, variables | M1, M2 | `vs_feature`: extrude, revolve, datums, equations |
| M4 | Remaining features | M3 | Sweep, loft, shell, pattern, fillet, chamfer |
| M5 | Volume model, connection surfaces, project files | M3 | `vs_model`, `.vsproj` format |
| M6 | Layout validation and diagnostics | M5 | `vs_validate` |
| M7 | Seam library and welding | M5 | `vs_mesh` seams, weld, mesh types |
| M8 | Conforming B-rep tessellation and mesh export | M7 | Watertight combined mesh, STL/3MF export |
| M9 | Evaluation engine | M6, M8 | `vs_eval`, static and parametric volumes, imports |
| M10 | Procedural graph core and fields | M9 | `vs_proc` graph, field nodes |
| M11 | Field-to-mesh against seams | M10, M7 | Surface-nets mesher, mesh-to-field |
| M12 | Procedural node library and baking | M11 | Lattices, TPMS, shell, scatter, instancing, outputs |
| M13 | App shell and viewport | M9 | Qt app that opens, shows and validates projects |
| M14 | Sketcher and feature editing UI | M13, M4 | Authoring layout and parametric volumes by hand |
| M15 | Node editor UI | M13, M12 | Editing procedural graphs with node previews |
| M16 | Interactive regeneration and guidance | M14, M15 | Drag previews, stale display, cancel, habit prompts |
| M17 | Ship workflow and MVP acceptance | M16 | Import/export UI, STEP export, end-to-end MVP test |

```
M0 ─┬─ M1 ─┐
    └─ M2 ─┴─ M3 ─┬─ M4 ──────────────────────────┐
                  └─ M5 ─┬─ M6 ─┐                  │
                         └─ M7 ─┴─ M8 ─ M9 ─┬─ M10 ─ M11 ─ M12 ─┐
                                            └─ M13 ─ M14 ───────┼─ M16 ─ M17
                                                     └─ M15 ────┘
```

Milestones on separate branches of this graph can run in parallel sessions: M1 and M2; M4 alongside M5–M8; M13 alongside M10–M12.

---

## M0 — Foundations and build

**Goal.** A repo any later session can build, test and extend on Windows and Linux without setup surprises.

**Scope**
- CMake presets for Windows (MSVC, Ninja) and Linux (GCC/Clang). vcpkg manifest with OCCT, Eigen, Manifold, Catch2, BLAKE3, fmt, spdlog, nlohmann-json. Qt is optional behind a `VS_BUILD_APP` option, so headless builds skip it.
- Directory layout from "Architecture at a glance", with an empty library for each module so later milestones only fill them in.
- `vs_core`: FP64 and FP32 vector, matrix and transform types; axis-aligned boxes; units (mm default); a `ProductSettings` struct holding the working-space size and tolerance; stable 128-bit IDs; a deterministic hasher with explicit, endian-independent serialisation of floats; a `Diagnostic` type (severity, code, message, list of subject references) used by every later module.
- `vstudio-cli` with `--version` and `selftest`.
- `CLAUDE.md` with build commands, conventions from this plan, and the module dependency rule.
- A script that runs the full build and test suite in one command on each platform.

**Acceptance**
- Clean clone builds and runs the tests on Windows and Linux with one command each.
- The hasher gives identical hashes on both platforms for a fixed set of test values (golden values checked in).
- The module dependency rule is checked at configure time (a small CMake check fails the build on a forbidden link).

**Out of scope.** Any geometry beyond math types. CI on GitHub Actions (optional; add if cheap).

## M1 — Kernel interface over OCCT

**Goal.** A small kernel API the rest of the studio uses for B-rep work, and an answer to the spec's first open question.

**Scope**
- `vs_kernel` public API in our own types: `Solid`, `Face`, `Edge`, `Shell` handles; primitives (box, cylinder, sphere, prism from a planar profile, revolve from a profile); booleans (union, cut, common); transforms; mass properties (volume, centre of gravity); bounding boxes; minimum distance between shapes; point-in-solid classification; validity check (`BRepCheck`) and a per-entity tolerance audit against the global tolerance.
- Error handling: every kernel call returns a result or a `Diagnostic`, never throws OCCT exceptions upward. Fallback messages for boolean and fillet failures.
- STEP AP242 and IGES import/export through OCCT, exposed as functions that produce or consume kernel solids.
- **Tolerance spike.** A test suite that builds and booleans parts near the edges of the ±1,000 mm box at 1e-6 mm, measures how OCCT's per-entity tolerances grow, and records the finding in `docs/decisions/0001-working-space-and-tolerance.md`, with a recommendation if the proposed values don't hold.
- `vstudio-cli kernel-demo` builds a sample part and writes STEP.

**Acceptance**
- No OCCT header is included from any `vs_kernel` public header (checked by a test that compiles the public headers without OCCT on the include path).
- Boolean, mass-property and distance tests pass against analytic answers within tolerance.
- STEP round trip of a sample part preserves volume to 1e-6 relative.
- The tolerance decision record exists and states whether the open question is settled.

## M2 — Sketches and constraint solving

**Goal.** Constrained 2D sketches that solve reliably and explain themselves when they don't.

**Scope**
- Vendor PlaneGCS from FreeCAD into `third_party/planegcs` with its own CMake target (Eigen dependency), and a `SketchSolver` interface in `vs_sketch` that hides it.
- Sketch data model: points, lines, arcs, circles, construction geometry; constraints for coincident, horizontal, vertical, parallel, perpendicular, tangent, equal, distance, angle, radius/diameter, symmetric, fix; dimensions that can hold expressions (resolved by M3; for now plain numbers).
- Solve results: solved, under-constrained (with remaining DOF count and the free entities), over-constrained (with the conflicting or redundant constraints named). These map onto `Diagnostic`.
- Drag solving: move one point with the rest of the sketch following, for the interactive editing in M16.
- Profile extraction: find closed loops of non-construction geometry, nest them into outer loops and holes, and produce planar faces through `vs_kernel`.
- JSON serialisation of sketches with stable IDs.

**Acceptance**
- A suite of canonical sketches (rectangle, slot, gear-like profile, bolt circle) solves to known values.
- Conflicting and redundant constraints are reported by name on deliberately broken sketches.
- Profiles become valid faces with holes in the right places.
- No PlaneGCS header appears in `vs_sketch`'s public headers.

## M3 — Feature tree, core features and variables

**Goal.** History-based parametric modelling, enough to build simple boundaries.

**Scope**
- Feature tree with ordered features, suppression, and rebuild from the top or from a changed feature.
- Reference geometry: datum planes (offset, three-point, through axis at angle), datum axes, datum points. Sketches can be placed on datum planes or planar faces.
- Features: extrude (blind, through-all, up-to-plane, symmetric, with draft), extruded cut, revolve, revolved cut.
- Global variables and equations: a small expression language (arithmetic, functions, units, references to other variables), a dependency order, and loop detection that names the full loop.
- Reference handling: features refer to sketches, datums and variables by ID. References to result faces store enough to re-find them after rebuild (a simple, documented strategy is fine; the spec accepts topological-naming fragility inside a volume). Lost references produce a "broken reference" diagnostic naming the feature.
- Tolerance audit after each feature (from M1).
- `vstudio-cli build <file>` rebuilds a feature tree from JSON and writes STEP.

**Acceptance**
- A test part driven by three variables rebuilds correctly when each variable changes.
- An equation loop is rejected when authored, with the loop given as a path.
- Deleting a referenced sketch entity yields a broken-reference diagnostic on the right feature, and features before it still rebuild.

## M4 — Remaining features

**Goal.** The rest of the MVP feature set.

**Scope**
- Sweep along a path (with simple orientation control), loft between two or more profiles, shell (remove faces, uniform thickness), linear and circular patterns of features, fillet and chamfer on edges.
- Validation and clear fallback errors for the OCCT weak spots (fillet and shell failures), as the spec's kernel table requires.
- Extend the reference strategy to these features.

**Acceptance**
- Each feature has tests on representative parts, including failure cases that produce a named diagnostic rather than a crash or invalid solid.
- Patterned features rebuild when the pattern count variable changes.

## M5 — Volume model, connection surfaces and project files

**Goal.** The volume contract in code: the data model every later milestone builds on.

**Scope**
- `Document` with a root layout (a feature tree) and a volume tree with one level of nesting under it.
- `Volume`: ID, name, kind (static, parametric, procedural), boundary (a closed solid from layout features or reference geometry), keep-out flag, contents slot, parameters, seed, published outputs, status.
- Keep-out regions: authored cuts that subtract from a volume's boundary before contents generate.
- `ConnectionSurface` as a named object owned by the common parent: planar patch geometry built from sketch entities or datums, the volumes it joins, a type with typed parameters (bonded; clearance with gap; bolted flange with hole pattern, thread size and minimum wall), and a derived seam definition placeholder (filled in by M7).
- Reference resolver enforcing encapsulation: only boundary, connection-surface and published-output references can cross a volume boundary. Attempts to reference contents fail at authoring time.
- Model-wide dependency graph (variables, features, volumes, published outputs) with loop rejection at authoring time, reporting the loop as a path.
- Parametric volume contents: each holds its own feature tree whose sketches can anchor only to its boundary and connection surfaces.
- Project file format `.vsproj`: a folder or zip with a JSON manifest plus binary blobs (meshes, imported files, optional caches), versioned from day one.
- `vstudio-cli info <project>` prints the volume tree, connection surfaces and dependencies.

**Acceptance**
- A sample "bracket" project (layout, two sibling volumes, one bolted-flange connection) saves and loads with identical hashes.
- Tests prove that every attempt to reference a volume's contents from outside is rejected with a diagnostic.
- A published output feeding something upstream of its volume is rejected with the loop path.

## M6 — Layout validation and diagnostics

**Goal.** Catch layout errors at the parent before anything generates, and route every diagnostic to one actionable place.

**Scope**
- Sibling overlap in FP64 via kernel `common`, reporting both volumes, the overlap region and its depth.
- Undeclared contact: sibling faces within tolerance with no shared connection surface raise a warning naming both faces.
- Structural adjacency: siblings sharing a connection surface are adjacent by definition, with no distance test.
- Connection type mismatch: per-type parameter rules (for example, the bolted flange's minimum wall against the hole pattern), naming the surface and conflicting parameters.
- Containment check as a reusable function (B-rep and mesh inputs) for M9 to call after generation, reporting the region outside the boundary.
- A diagnostics registry: each diagnostic has a code, severity and a single "act here" target (layout, connection surface, feature, or node).
- `vstudio-cli validate <project>` prints diagnostics and exits non-zero on errors.

**Acceptance**
- Fixture projects for each check (overlap, contact, mismatch, loop, containment) produce exactly the expected diagnostics.
- Validation of a 50-volume layout completes in well under a second.

## M7 — Seam library and welding

**Goal.** The shared seam library the spec calls out as the top risk, built and heavily tested before any mesher depends on it.

**Scope**
- `vs_mesh` mesh types (FP32 positions, indexed triangles, per-triangle volume ID) and half-edge utilities for manifold and watertight checks.
- Seam definition for planar connection surfaces: the outline is divided into edges and vertices at a spacing set per surface (derived automatically, overridable), stored in FP64 and shared by every volume touching the surface.
- Seam discretisation API that every mesher must use: given a seam and a target edge length, return the exact shared polyline. The interior of the patch gets a shared constrained triangulation too, so both sides mesh the connection face identically.
- Welding: concatenate per-volume meshes and merge vertices by seam vertex ID (structural, not by distance), then verify watertight and manifold.
- Property-style tests: random planar outlines, random spacings, both sides meshed independently, weld always succeeds.

**Acceptance**
- Weld succeeds structurally on every generated test case, and the result passes watertight and manifold checks.
- A deliberately non-conforming mesh fails welding with a diagnostic naming the connection surface.

## M8 — Conforming B-rep tessellation and mesh export

**Goal.** Watertight combined meshes from static and parametric volumes, which answers the spec's open question about the tessellator.

**Scope**
- A custom tessellation pass for B-rep volumes: faces lying on connection surfaces use the M7 seam triangulation; other faces use OCCT's `BRepMesh` with their boundary edges pre-discretised to match seam vertices where they meet. Document what worked in a decision record.
- Combined mesh: tessellate each volume, concatenate, weld.
- Export: STL (whole design) and 3MF (whole design, or one object per volume with names and metadata) through `lib3mf` or a small writer.
- Import: STL, 3MF and OBJ into static mesh volumes.
- `vstudio-cli export <project> --3mf/--stl`.

**Acceptance**
- The bracket project exports a single watertight, manifold 3MF that opens cleanly in a slicer (checked manually once; automated by our own manifold check).
- Tessellation tests on curved faces adjacent to seams pass the weld.
- Import of a sample STL gives a static volume with correct triangle count and bounds.

## M9 — Evaluation engine

**Goal.** One scheduler for every kind of volume, with caching, parallelism, failure isolation and determinism checks.

**Scope**
- The evaluation order from the spec: rebuild authored features on the calling thread; validate layouts; schedule changed volumes; containment-check results; publish outputs; weld and notify.
- Cache key: hash of boundary, connection surfaces, parameters, seed, content definition and engine version. In-memory cache plus optional on-disk cache in the project.
- Deterministic thread pool: work split into fixed-size chunks with ordered reductions, so results don't depend on thread count.
- Volume states: current, stale, regenerating, failed. A failed volume keeps its last valid contents marked stale and blocks only its dependants.
- Cancellation tokens and progress reporting for long jobs.
- Preview tier: a cheap "boundary only" result available immediately for any volume.
- Static volumes (imported files, never regenerate) and parametric volumes (rebuild their feature tree) plugged in as the first two content kinds. STEP/IGES import becomes static volumes with containment checks.
- Determinism check: a debug flag that regenerates each volume twice and compares output hashes.
- `vstudio-cli rebuild <project> [--threads N] [--check-determinism]`.

**Acceptance**
- Changing one volume's parameter regenerates only that volume and its dependants (counted in tests).
- Output hashes are identical with 1, 4 and 16 threads.
- A volume forced to fail leaves siblings current and its own contents marked stale.
- A cancelled job leaves the model in a consistent state.

## M10 — Procedural graph core and fields

**Goal.** The node graph runtime, its type system, and field evaluation, with no meshing yet.

**Scope**
- Graph model: typed ports (field, mesh, point set/frames, curves, scalar, vector, list), nodes, edges, exposed parameters, seed. Type checking at authoring time; conversions only through explicit nodes.
- Node API: a node declares ports, evaluates deterministically from its inputs and a derived seed, and can report infeasibility as "these inputs can't all be met" plus node-failure diagnostics naming the node and inputs.
- Graph inputs limited to what the spec allows: boundary, connection surfaces with type and parameters, parameters, seed.
- Field representation: analytic signed-distance trees evaluated lazily, with sampling onto a regular FP32 grid clipped to the boundary's bounding box.
- First node families: boundary access (boundary as SDF, computed from the B-rep), connection-surface access, field primitives (sphere, box, cylinder, plane, half-space), field booleans (union, intersection, difference, smooth variants), queries (distance to boundary, distance to a connection surface), math and logic, parameter nodes.
- Procedural volumes join the M9 scheduler; graph evaluation is per-node cached by input hash.
- JSON serialisation of graphs.

**Acceptance**
- A graph "boundary shelled by 2 mm, unioned with a sphere" evaluates to a sampled field matching analytic values.
- Type errors and graph loops are rejected at authoring time.
- Graph evaluation is identical across thread counts and runs.

## M11 — Field-to-mesh against seams

**Goal.** Turn fields into meshes that respect the boundary and weld at every seam: the second half of the seam-conformity risk.

**Scope**
- Surface-nets mesher on the sampled grid, FP32 output, deterministic vertex ordering.
- Boundary handling: intersect the field with the boundary SDF so output stays inside within tolerance; on faces lying on connection surfaces, replace the grid-derived surface with the shared seam triangulation from M7 and stitch the field mesh to the seam polyline.
- Mesh-to-field conversion node (distance evaluation with a BVH), and the explicit `Field → Mesh` node with a resolution parameter whose cost is visible.
- Containment check on generated meshes via M6.

**Acceptance**
- A procedural volume next to a parametric volume welds into one watertight mesh across their shared connection surface.
- Containment holds for random fields clipped to random boundaries.
- Mesh output hashes are identical across runs and thread counts.

## M12 — Procedural node library and baking

**Goal.** The MVP node families that make procedural infill useful for 3D printing.

**Scope**
- Lattices (strut lattices on cubic, BCC and octet cells as fields) and TPMS (gyroid, Schwarz P, diamond) with cell size and thickness parameters, gradable by a scalar field.
- Offset, shell and thicken on fields.
- Scatter (seeded, deterministic points in the volume or on a connection surface) and pattern (linear, circular, grid of frames).
- Instancing: place a mesh template or a small parametric template at frames.
- Mesh booleans through Manifold (union, difference, intersection) as nodes.
- Published outputs: mass (with a density parameter), volume, centre of gravity, bounding envelope, counts.
- Node output inspection API: any node's output can be evaluated and returned for preview (the UI in M15 draws it).
- Baking a procedural volume into a static volume, and unbaking it when the graph is kept.
- A starter template library: gyroid infill with shell, graded lattice, bolt-boss pattern on a bolted flange.

**Acceptance**
- Each template generates, passes containment, welds and exports to 3MF.
- Bake then unbake round-trips the volume's content hash.
- A large gyroid infill (100 mm cube at 0.5 mm resolution) completes in a time recorded in the PR as the CPU baseline for the GPU milestone.

## M13 — App shell and viewport

**Goal.** A Qt application that opens, shows, validates and exports projects built by the CLI, which proves the GUI foundation before any editing.

**Scope**
- Main window with dockable panels: viewport, volume tree, feature tree (read-only for now), inspector, diagnostics list.
- Viewport using OCCT's AIS in a Qt widget: B-rep display, mesh display for generated contents, tinted volume boundaries, connection surfaces coloured by type, orbit, pan, zoom and view cube.
- Volume tree with kind icons and status badges (current, stale, regenerating, failed), wired to the M9 engine on a background thread with results delivered to the UI thread.
- Selection model: selecting in the tree highlights in the viewport and the reverse; the inspector shows parameters, connection-surface properties and published outputs.
- Diagnostics list: clicking a diagnostic selects its "act here" target.
- File open, save, recent files; export to STL and 3MF from menus.

**Acceptance**
- The sample projects from M5–M12 open, display correctly and export from the app.
- The UI stays responsive (no frame over 100 ms) while a long procedural volume regenerates.

## M14 — Sketcher and feature editing UI

**Goal.** Author layouts and parametric volumes by hand: the "block out" and "fill by modelling" workflows.

**Scope**
- Sketch mode: draw lines, arcs, circles, rectangles and slots on a plane; add constraints and dimensions; live solving with DOF shown; conflicts highlighted by name.
- Feature dialogs for every M3/M4 feature with live preview; edit, reorder, suppress, delete in the feature tree; variables and equations table.
- Volume authoring: create a volume from a solid, mark keep-outs, create connection surfaces from sketch entities or datums and set type parameters.
- Edit in context: open a parametric volume to edit its own feature tree, with outside geometry dimmed and only its boundary and connection surfaces selectable as references.
- Undo/redo for all model edits (command pattern over the document).

**Acceptance**
- The bracket project can be rebuilt from scratch entirely in the app.
- Undo/redo restores identical document hashes.

## M15 — Node editor UI

**Goal.** Build and inspect procedural graphs: the "fill by procedure" workflow.

**Scope**
- Node editor panel (QtNodes or a small custom canvas) with typed, colour-coded ports, type-checked connections, a searchable node palette, parameter editing in the inspector, and exposed parameters.
- Node preview: pick any node and draw its output in place inside the volume in the viewport (fields as an iso-surface or slice, point sets as glyphs, meshes as meshes).
- Node diagnostics on the canvas: failed and infeasible nodes marked, with messages naming the inputs.
- Template library browser to attach a template to a volume; bake and unbake actions.

**Acceptance**
- The M12 templates can be built from scratch in the editor and produce the same hashes as their saved versions.
- Previewing any node in a 50-node graph shows within a second when its result is cached.

## M16 — Interactive regeneration and guidance

**Goal.** The "iterate" and "swap" workflows: editing feels live, and CAD habits get redirected rather than blocked.

**Scope**
- Dimension dragging in the viewport and sketcher with M2 drag solving; affected volumes show boundary-only or low-detail previews during the drag, and full generation starts when the drag ends.
- Stale results drawn greyed out until replacements arrive; a progress indicator and cancel for long generations.
- Swap content kind (placeholder → procedural → baked) from the volume's context menu, with nothing outside the volume touched (verified by hash in tests).
- Habit guidance: selecting a generated face and choosing fillet, cut or mate shows an inline explanation with the right action (edit procedure parameters, add a keep-out, or use the connection surface).
- Face and edge picking in the viewport (deferred from M14, decision 0014): B-rep items keep their per-face triangle ranges as separately pickable parts, a picked face resolves to its FaceRef name through the rebuild's named faces, and feature dialogs get a "pick in view" mode that fills face and edge fields from clicks (edit-in-context rules still apply; generated meshes stay unpickable). The labelled lists stay as the fallback.

**Acceptance**
- Dragging a layout dimension on the bracket project keeps the viewport above 30 fps with previews.
- Every reference a feature dialog takes on the bracket (planes, faces, edges for a fillet) can be chosen by clicking in the viewport, in the layout and in context, and resolves to the same reference the list would give.
- Swapping a volume's contents leaves every other volume's cache key unchanged.

## M17 — Ship workflow and MVP acceptance

**Goal.** Close out the MVP: import and export from the app, STEP output, and an end-to-end test of the spec's five workflows.

**Scope**
- STEP AP242 export: layout, boundaries and parametric volumes as B-rep; procedural volumes as mesh bodies or boundary only (user's choice).
- Import STEP/STL into a volume from the app, with containment diagnostics.
- A "Validate" command that runs every check and shows a summary before export.
- Native project caches: optional saving, rebuild on load when missing or stale.
- An end-to-end scripted test that walks block out → fill → iterate → swap → ship through the document API, and a written manual test script for the same workflows in the GUI.
- Fix the gaps the end-to-end run uncovers, and update this plan's status table and the spec's open questions.

**Acceptance**
- Every MVP bullet in the spec's "Scope and phasing" section maps to a passing test or a checked manual step.
- A real part designed in the app prints successfully from its exported 3MF.

---

## After the MVP

These follow the spec's "Later" list, roughly in order of value. Each will need its own short plan when it's reached.

| # | Milestone | Notes |
| --- | --- | --- |
| L1 | GPU field evaluation | Compute shaders for field sampling with ordered reductions or quantised hashing; must match CPU hashes or document the quantisation. Plan: `docs/plans/L1-gpu-field-evaluation.md`. **Done (#22):** exact, no quantisation: GLSL FP64 with a correctly rounded sqrt gives the CPU's samples bit for bit; sampling 2.8x faster than 4 CPU threads on the 100 mm gyroid, but meshing now dominates (decision 0018) |
| L1b | Faster meshing | Surface nets (18 of the 26 s of the 100 mm gyroid after L1, all on one thread) made parallel on the CPU with exactly the same mesh (slab passes, the per-cube merge by wavefronts), and the clip and unmerged net on the GPU where it measures faster. Plan: `docs/plans/L1b-parallel-and-gpu-meshing.md`. **Done (#24):** the same mesh bit for bit; engine 26.0 s to 10.8 s on the 100 mm gyroid (6-core desktop); GPU clip and unmerged net exact, used per stage only where calibration finds the GPU faster (decision 0019) |
| L2 | Curved connection surfaces | Authored or derived seam definitions on curved patches (spec open question). Plan: `docs/plans/L2-curved-connection-surfaces.md`. **Built (#25):** cylindrical patches (curved rectangles and bands round a layout axis), bonded or clearance; seams derived in unrolled coordinates with the spacing authored (chords within 0.01 mm when derived); B-rep volumes conform by imprinting the patch, procedural ones by the same stitch through the seam's surface; planar results unchanged (decision 0020) |
| L3 | Composite volumes and deep nesting | Child volumes authored at any depth. Plan: `docs/plans/L3-composite-volumes.md`. **Built:** a composite's contents are its own layout (a feature tree rebuilt with its inputs) plus which child meets each surface joining it; children stay in the one flat volume list with a `parent`, so flat documents and every hash are unchanged; encapsulation, overlap and contact hold per level, with `validate.nesting.outside` for a child outside its parent; delegated surfaces weld leaves at any depth structurally; composites never generate, take the worst of their children's statuses and add their measures up; the app nests the volume tree and edits a composite's layout in context (decision 0021) |
| L4 | Procedures that emit child volumes | Generated layouts pass the same overlap and containment checks. Plan: `docs/plans/L4-generated-child-volumes.md`. **Built:** a composite may have a generator (a graph ending in `output.children`) and templates; `children.grid`, `.at_frames`, `.merge` and `.delegate` emit boxes, joins between them and delegations; the engine expands them into the evaluated document as ordinary L3 children (derived ids, never saved, read-only), so overlap, nesting, containment and the weld apply unchanged, and a generated layout that fails a check fails its composite; parametric templates are rebuilt at an offset so one template fills any box; Make template turns an authored child into one (decision 0022) |
| H | Hardening | Verified bug fixes from an audit, before L5: CI sanitizers, design-rule bypasses, determinism and cache keys (`kEngineVersion = 2`), mesh and file I/O, memory and cancellation on Linux first, then the GPU module and the app on the Windows desktop. FP32 fields were proposed and cut. Plan: `docs/plans/H-hardening.md`. **Done**: H1 done (#31, decision 0023), H2 done (#32, decision 0024), H3 done (#33, decision 0025), H4 done (#34, decision 0026), H5 done (#35, decision 0027), merge memory follow-up done (#36, decision 0028), leaner edits done (#37, decision 0029), rack benchmark done (#38, decision 0030), document index done (#39, decision 0031), box layout checks done (#40, decision 0032), layout build done (#41, decision 0033), lean layout checks done (#42, decision 0034), H6 GPU fixes done (#43, decision 0035), H7 app fixes done (#44, decision 0036) |
| L5 | Configurations and design tables | Drive procedure parameters and variables. Plan: `docs/plans/L5-configurations-and-design-tables.md`. **Done:** named configurations in the document give values (expressions) to layout variables, composites' layout variables and volume parameters, the numbers a link may drive; the active one is saved and is what build, validate and the engine build; `check()` refuses any configuration that wouldn't build, so switching never fails; cache keys see only values, so switching back regenerates nothing; a design table is the configurations as CSV (`vstudio-cli design-table`, `configs --build`); the app's commands and data are in `vs_app_core`; the app has a Configuration box on the toolbar, Model > Design Table… (rows, columns, CSV import and export) and inspector marks for driven numbers (decision 0037) |
| L6 | Drawings | Disposable dimensions on generated contents. Plan: `docs/plans/L6-drawings.md`. **Done:** drawings in the document (sheet, views, dimensions between anchors on the origin, boundaries, connection surfaces or contents; only the definition is saved, and nothing that builds sees it); a new `vs_draw` library projects views with its own hidden-line removal for B-reps and meshes, measures stable dimensions (boundaries, surfaces) and disposable ones (contents, in parentheses, dropped with a warning when gone) from every evaluation, and writes SVG, PDF and DXF; `vstudio-cli drawing` with `--standard`; the app has a Drawings dock, a drawing window, view and dimension dialogs, Model > New Drawing and File > Export Drawing… (decision 0038) |
| U1 | Context toolbar | Tools follow the context (Layout, Contents, Sketch, Graph, Drawing) in one labelled toolbar under a tab row; one Browser dock; the Diagnostics dock becomes a status-bar count and a Problems drawer under the viewport. Plan: `docs/plans/U1-context-toolbar.md`. **Done:** phases 1 to 6 (the Qt-free tool catalogue, availability rules with reasons, stroke icons; one shared action per tool and the context bar in the window, the Insert, Volume and Model menus gone; sketch mode driven by the Sketch tab; one foldable Browser and a foldable Inspector; a Problems drawer under the viewport with a status-bar count; drawings in the main window, one theme, panels remembered between runs; decision 0039) |
| S1 | Bézier curves and SVG import | SVG paths imported into sketches as outlines to constrain and extrude. Plan: `docs/plans/S1-bezier-curves-and-svg-import.md`. **Phase 1 done:** a cubic Bézier sketch entity whose control points are points (Tangent at shared ends, Fix), sampled like arcs in profiles, exact `Geom_BezierCurve` edges in faces, sweeps, revolves and lofts, and extruded sides named by sampled surface points so fillets, chamfers and patterns follow them (decision 0040). **Phase 2 done:** `import_svg()` and `vstudio-cli sketch-import-svg`: paths, shapes, transforms and `use` in mm, ends joined within a gap, curves off loops as construction, smooth joints as tangents, Fix without redundancy; the XML reader moved to vs_core. Next: the app's Curve and Import SVG tools, then Béziers in the solver |
| L7 | Assemblies and mates to connection surfaces | |
| L8 | Approximate mesh-to-B-rep | Flagged as approximate |
| L9 | Automatic easing of layouts | Convenience layer over the fixed-input rule |
| L10 | User-defined nodes (scripting) | Once the app is basically stable |
| L11 | Cloud generation for heavy procedures | Builds on headless libraries and L1 |

## Spec open questions and where they're answered

| Open question | Answered in |
| --- | --- |
| Does OCCT's tolerance model fit ±1,000 mm at 1e-6 mm? | M1 tolerance spike |
| Final working-space size and tolerance | M1 decision record |
| Connection surface types and parameters for the MVP | Bonded, clearance (gap), bolted flange (hole pattern, thread, minimum wall): M5, decision 0005 |
| Authored or derived seams on curved connection surfaces | Derived, with the spacing authored: L2, decision 0020 (cylinders; bolted flanges stay planar) |
| Can the tessellator respect fixed seam edges on B-rep faces? | Yes: M8, decision 0008 |
| Single field-to-mesh algorithm or per node? | Single (surface nets) for the MVP, in M11 |
| How published outputs are versioned | M9: outputs carry the producing volume's cache key, so dependants see a change whenever it changes |

## Status

| # | Status | PR | Notes |
| --- | --- | --- | --- |
| M0 | Done | #1 | OCCT, Eigen, Manifold and Qt are optional vcpkg features (decision 0000) |
| M1 | Done | #2 | OCCT 8.0.1; tolerance findings in decision 0001 |
| M2 | Done | #3 | PlaneGCS from FreeCAD 1.1.4 in `third_party/planegcs`, built without Boost |
| M3 | Done | #5 | Fuzzy feature booleans and geometric face references (decision 0002) |
| M4 | Done | #6 | Edges found by their faces, cylindrical face names, fallback search for fillets, chamfers and shells (decision 0003) |
| M5 | Done | #7 | First milestone from a cloud session. Connection surface types fixed, contents see inputs through `ExternalInputs`, stored-only zips (decision 0005; deflated since M6) |
| M6 | Done | #8 | Overlap depth is the region's thinnest side, edge-only touches warn, a fifth routing target (contents), meshes checked through a view, miniz vendored for deflated zips (decision 0006) |
| M7 | Done | #9 | Spacing set per surface (derived: a sixteenth of the diagonal), own constrained Delaunay triangulator, clearance surfaces get no seam, flange bolt holes are seam holes, weld removes paired seam faces (decision 0007) |
| M8 | Done | #11 | BRepMesh's points, re-triangulated where seams meet faces; vertices between seam vertices snap; seams carry hole fills for holes a solid doesn't have; own 3MF writer and reader over miniz (decision 0008); the bracket's 3MF was opened in a slicer by hand on Nov 19, 2026 and loaded cleanly |
| M9 | Done | #12 | `build()` split into layout and contents halves; keys hold the boundary's exact shape, and static keys leave parameters and seed out; links scheduled in waves; containment on the calling thread; failed results cached; disk cache of BRep text (decision 0009) |
| M10 | Done | #13 | The engine takes content generators, so `vs_proc` plugs in from above; fields are lazy expression trees sampled to FP32 on the boundary's box; the boundary SDF is its tessellation with pseudonormal signs; authoring rejects type errors and loops but allows unfinished graphs; per-node cache by input hash (decision 0010) |
| M11 | Done | #15 | Surface nets on the grid's tetrahedra, merged per cube where the link condition allows (always manifold, about two triangles a cell); the field is clipped 0.01 mm inside the boundary's tessellation and vertices move inward until every vertex and centroid is contained; seams replace the patch's grid triangles and are stitched to the rim by an arc-length strip, or a warning says the contents don't cover the patch; the engine checks and welds generated meshes (decision 0011) |
| M12 | Done | #16 | Lattice and TPMS fields with deterministic sine and cosine, gradable; scatter and pattern frames; parametric templates are field templates (jobs can't use the kernel) and meshes are graph tools that reach contents through mesh-to-field; Manifold booleans in a canonical order; measured and graph-published outputs; node inspection; bake keeps the graph and the seam vertices; 100 mm gyroid at 0.5 mm: 23.9 s on 4 threads, 31.7 s on 1 (decision 0012) |
| M13 | Done | #17 | Everything drawn as meshes (B-reps tessellated by vs_kernel) with OCCT's visualisation toolkits only; one worker thread delivers a preview then the full evaluation; engine pool leaves two cores to the UI; big meshes uploaded a part per frame; 862k-triangle regeneration keeps UI gaps under 40 ms (decision 0013) |
| M14 | Done | #18 | Undo and redo store whole project snapshots, so hashes come back exactly; sketch mode is a 2D canvas in place of the viewport; references are picked from lists computed for the tree being edited (inside a volume: only its boundary faces and connection surfaces); dialogs preview edits on a copy; model errors are refused, rebuild failures kept; the bracket rebuilt in the app in 29 steps (decision 0014) |
| M15 | Done | #19 | A custom canvas on QGraphicsView; every gesture is one undoable `set_graph()` command through `proc::edit()`; node positions kept per session, not in the graph; previews on their own thread with the generator's node cache and cached inputs (50-node graph: slowest preview 115 ms); fields as a surface or a coloured slice; a node form in the inspector; marks from diagnostics' subjects (decision 0015) |
| M16 | Done | #20 | Handles drag what drives a dimension (a variable, else the number; none for longer expressions), previewed by the preview tier alone with changing volumes as boundaries (bracket drag: 60 fps); procedural jobs stop when cancelled; swaps between placeholder, modelled, procedural and baked change only that volume; habit guidance is an inline bar with one action; faces are pickable parts of B-rep items, named by matching their surface with the listed references, clicking again picks what's behind (decision 0016) |
| M17 | Done | #21 | One STEP assembly of named parts: B-rep contents as solids, procedural and mesh contents as AP242 tessellated mesh bodies (or boundaries, by choice), placeholders as boundaries, only current results; File > Import as New Volume; Validate is every check against what is shown (validate(), generation, stale volumes, the weld) and runs before every export; caches (now meshes too) travel inside folder and zip projects by key, parametric contents rebuild; the five workflows end to end in `[mvp]`, the GUI walk and the MVP mapping in `docs/manual-tests/mvp-workflows.md` (decision 0017). Print acceptance: Dallas checked an exported 3MF in a slicer (Nov 20, 2026), which stands in for a printed part for now |

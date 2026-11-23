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

Presets: `windows-msvc-{debug,release,app}`, `linux-{gcc,clang}-{debug,release}`,
`linux-clang-asan` (AddressSanitizer + UndefinedBehaviorSanitizer, plan H1).
Release presets are `RelWithDebInfo`. Build trees go to `build/<preset>/`.

### Options and vcpkg features

| CMake option | Default | Effect |
| --- | --- | --- |
| `VS_BUILD_APP` | OFF | Builds the Qt app (`vs_app_ui`, `vs_app`, `vs_app_tests`) and turns on the vcpkg `app` feature (Qt 6). Only the `windows-msvc-app` preset sets it. `vs_app_core` and `vs_app_core_tests` build either way. |
| `VS_BUILD_TESTS` | ON | Builds the Catch2 suites and registers them with CTest. |
| `VS_WARNINGS_AS_ERRORS` | ON | `/WX` or `-Werror` on first-party code. |
| `VS_SANITIZE` | empty | Sanitizers for everything built here (not vcpkg's ports), e.g. `address;undefined`; GCC and Clang. `linux-clang-asan` sets it; LeakSanitizer suppressions are in `scripts/sanitizers/lsan.supp`. |

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

### Prebuilt dependencies on a second Windows machine (GPU VM)

The desktop publishes its Windows vcpkg archives (OCCT, Qt, ...) to the
`vcpkg-cache-x64-windows` prerelease with `scripts\vcpkg-cache.ps1 publish`;
another machine pulls them with `fetch`. They only hit with the exact MSVC
build pinned in `scripts\windows-toolchain.json` and the same vcpkg
baseline, so assets are named by that key (`vcpkg-cache.ps1 key`), and
`build.ps1` warns when the local toolset isn't the pinned one. After a new
dependency, a baseline bump or a Visual Studio update, rebuild the app
preset on the desktop and publish again. `scripts\gcp\New-DevVm.ps1`
(or `scripts/gcp/new-dev-vm.sh` from macOS or Linux: bash 3.2 and BSD
tools, `--dry-run` to check first) creates a Compute Engine GPU VM whose
startup script (`scripts\gcp\bootstrap-windows.ps1`) installs everything
and builds;
`docs/dev-vm.md` has the manual steps and decision 0019 the reasoning.

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
vs_core → vs_kernel → vs_sketch → vs_feature → vs_model → vs_validate → vs_mesh → vs_eval → vs_proc → vs_draw → vs_gpu
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
- **XML.** `vs/core/xml.hpp` is a small reader (element tags and
  attributes, prefixes stripped; `scan_xml()`, `escape()`, `unescape()`)
  shared by 3MF (vs_mesh) and SVG (vs_sketch); moved from vs_mesh in S1.
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
  passed `check_validity`; boxes and faces bounded by a strictly convex
  polygon without holes are valid by construction and skip it (decision
  0033, guarded by random shapes in `test_primitives.cpp`).
- **Settings in, not constants.** Builders take `ProductSettings` and reject
  sizes under the minimum feature size and shapes outside the working space.
  `audit_tolerances()` compares OCCT's per-entity tolerances with the
  product tolerance; call it after each feature (M3).
- **Threads.** Handles are immutable, but OCCT's classifiers and distance
  tools may fill caches on the shape they read: work on several threads
  gives each thread its own `deep_copy()` (H2). `SolidClassifier` isn't
  copyable; one per thread.
- **Sweeps, lofts, shells, chamfers** (M4) live beside the primitives and
  fillets. Booleans also take several tools at once. `shell()` rejects
  walls thicker than a curved face's radius up front, because OCCT returns
  a valid-looking inside-out solid for them.
- **Exchange.** STEP (AP242, mm) and IGES go through `exchange.hpp`; calls
  are serialised because OCCT's exchange layer has global state.
  `write_brep()` / `read_brep()` (OCCT's own text, no triangulations) and
  `shape_hash()` serve M9's cache keys and disk cache. Since M17,
  `write_step(parts, path, name)` writes an assembly of named parts
  (XDE) and `read_step_parts()` reads it back; `mesh_shape()` is a
  surface-less face carrying triangles, written as AP242 tessellated
  geometry (decision 0017).
- **Béziers** (S1): `BezierSegment` is an exact `Geom_BezierCurve` edge
  in profiles and paths. `surface_normals_at(face, points, tol)` says
  whether points lie on a face's underlying surface (any kind, searched
  past the trimmed face) and gives the normals there.
- **Tessellation** (`tessellate.hpp`, M8) runs BRepMesh on a copy and
  returns plain data: vertices, each edge's division, each face's wires
  and triangulation with where every point sits (inside, vertex, k-th edge
  point). It knows nothing of seams; `vs_mesh::tessellate()` builds on it.
- **Tolerance findings** are in `docs/decisions/0001-working-space-and-tolerance.md`;
  `vstudio-cli tolerance-spike` reruns the measurements.

## vs_sketch essentials

- **Model.** A `Sketch` holds entities (points, lines, circles, arcs,
  cubic Béziers) and constraints, each with a stable `Id`. Curves refer to
  point entities, so sharing a point joins two curves. Arcs run
  counter-clockwise from start to end. A `Bezier`'s four control points
  are point entities (S1, decision 0040): the solver sees only them, so
  Tangent with a Bézier needs a shared end (handle collinearity), and
  point on curve, Equal, Radius and length on one are refused until
  phase 4 puts curves in PlaneGCS. Dimensions carry a `value` and an optional `expression` (evaluated by
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
  turns them into `vs_kernel` faces on the sketch plane. Loops whose sides
  cross (`sketch.profile.loops_cross`, coincident loops too) or that cross
  themselves (`sketch.profile.self_crossing`) are refused; arcs and
  Béziers become polygons within the minimum feature size
  (`BezierGeometry::steps_within()`), compared by box and an x sweep (H2).
- **SVG import** (S1 phase 2, `svg.hpp`): `import_svg()` /
  `import_svg_file()` add an SVG file's path and shape outlines to a
  sketch (fills and styles ignored; transforms, viewBoxes and `use`
  flattened; units through width and height, px at
  `pixels_per_inch`; Y flipped). Ends within `gap_mm` share a point,
  across elements too; curves not on a loop come in as construction
  (`sketch.import.open`); circular arcs stay arcs (a full loop of them a
  circle), elliptical ones become Béziers (`.ellipse_approximated`).
  Smooth joints get Tangent; `fix` holds everything with nothing
  redundant. On error the sketch is unchanged. Fixtures in
  `tests/data/svg`; `vstudio-cli sketch-import-svg <file.svg> [--out
  sketch.json] [--scale k] [--dpi n] [--gap mm] [--fix] [--no-smooth]`.
- **Files.** `to_json()` / `sketch_from_json()` round-trip exactly, ids
  included; an object's own id may not be null (H2, also in vs_feature and
  vs_model files). `vstudio-cli sketch-solve <file>` solves one from the
  command line.

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
  it; reuse needs every feature above unchanged (`RebuildResult::built_from`)
  and the same `ProductSettings`, so a wrong start never reuses stale
  results (H2). An input value never stands in for a tree variable of the
  same name, even a failed one. Every feature boolean is fuzzy at the linear tolerance, and the
  tolerance audit runs after each body change (decision 0002).
- **Face references** name a face by feature and name (`start`, `end`,
  `side:<sketch curve id>`, `<k>:<seed id>:<name>` on pattern copies) and
  are found again by plane or cylinder, or for an extruded Bézier's side by
  sampled points and normals (`NamedFace::samples`, `curved()`; decision
  0040); see `rebuild.hpp`. **Edge
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
  (every depth, one flat list), and the connection surfaces, keep-outs and
  links. Ids are unique across the whole document, contents included.
  A `Volume` has a boundary (layout solid features' tools, or a box between
  two layout points), a keep-out flag, contents (static file, parametric
  tree, procedural graph JSON), parameters, a seed and published outputs.
  Static contents keep `baked_graph` when a procedural volume was baked
  (M12); `OutputQuantity::Published` takes a number the graph publishes.
- **Authoring.** Change a document through `edit(doc, fn)` (`check.hpp`): it
  applies `fn` to a copy and keeps it only if `check()` finds no errors.
  `check()` covers structure (`model.*`; null ids are
  `model.document.null_id`; `feature::tree_ids()` / `find_tree_id()` is
  the one walk over a tree's ids, sketch constraints included), encapsulation
  (`model.reference.contents` / `.sibling` / `.not_input` / `.parent`, all
  judged by `resolve()` in `references.hpp`) and dependency loops
  (`model.dependency.loop`, path with the link first, `dependency.hpp`;
  a feature using the body above it, such as a through-all extrude, a cut,
  shell, fillet, chamfer or pattern, depends on every body feature above
  it, H2).
- **Index** (`index.hpp`, decision 0031): `DocumentIndex` is a read-only
  index of one document (ids, children, joining surfaces, keep-outs,
  delegations, `classify()` targets) answering exactly what the list
  searches answer, first match winning. Anything that looks up per volume
  or surface (`check()`, `DependencyGraph::build(index)`, cache keys, the
  layout checks, the engine) builds one per pass; one list search per
  volume is quadratic at 10,000 generated children. The walks are written
  once over either (`src/lookup.hpp`); `test_index.cpp` compares them.
- **Contents see inputs, nothing else.** A parametric tree is rebuilt with
  `feature::ExternalInputs`: the boundary by the volume's id (faces
  `<layout feature id>:<name>`, or `-x`..`+z` for a box), each joining
  connection surface by its id (a plane, face `patch`; a curved one is
  its axis and a cylindrical `patch`), and parameters as
  values. Published outputs reach anything only through a `Link` to a layout
  variable or parameter.
- **Build.** `build(doc, settings, &blobs)` rebuilds the layout, boundaries
  (keep-outs cut), patches (with bolt hole centres) and contents, and
  measures outputs. Status (`VolumeStatus`) is on the result, never saved.
  It is `build_layout()` (layout, boundaries, patches, parameters) then
  `build_contents()` per volume, which only reads the layout and may run
  on several threads; vs_eval calls the halves itself, passing its pool
  to `build_layout(doc, settings, parallel)`, which builds each level's
  boundaries and surfaces on every thread into their own slots (the same
  build as on one thread; boundaries uniting several tools stay on the
  calling thread; `test_parallel_layout.cpp`, decision 0033). A layout reads
  only `layout_inputs(doc)` (leaf contents, outputs and seeds left out);
  `layout_shape()` also leaves out leaf parameter values, which
  `refresh_parameters()` redoes on a kept layout (decision 0029).
- **Files.** `.vsproj` is a folder or a zip of `project.json` plus
  `blobs/<hash>.<ext>` (`project.hpp`, decision 0005), and optional
  `cache/` files (`save_project(..., &cache)`, `load_project_cache()`;
  never hashed, in zips too since M17). Blobs are shared
  (`BlobPtr`, `std::shared_ptr<const Blob>`): copying a project copies
  pointers, and equality and hashes compare bytes (H5). Zips go through the
  vendored miniz (deflated, zero timestamps, decision 0006). Loading checks
  blob hashes and runs `check()` (`load_project(path, false)` skips it, for
  validation). `content_hash()` hashes the document's
  JSON plus the blobs. `vstudio-cli info <project> [--build]` prints one.
  `.gitattributes` keeps `*.vsproj/**` byte-exact; regenerate the samples
  (`bracket.vsproj`, `nested.vsproj`) with `vs_model_tests "[.generate]"`.
  The shared test models (`tests/bracket.*`, `curved.*`, `nested.*`,
  `rack.*`) are the `vs_model_test_fixtures` library: suites link it
  rather than compiling them again.
- **Composites** (L3, decision 0021, `hierarchy.hpp`): `Volume::parent`
  names a composite (null at the root; written to JSON only when set, so
  flat documents and their hashes are what they were). A **level** is the
  root or a composite; `CompositeContents` is its layout (a `FeatureTree`,
  rebuilt with the composite's inputs like a parametric tree, and what
  `Volume::tree()` returns) plus `delegations` (`{surface, child}`: which
  child meets a surface joining the composite). Children's boundaries,
  and the surfaces, keep-outs and links of a level, are built from its
  layout (`model.level.mixed` when one names volumes of two levels);
  `build_layout()` goes level by level, parents first (`level_order()`).
  `resolve()` treats "inside a composite" as its level: children's
  boundaries, parameters and outputs yes, anything deeper
  `model.reference.contents`, anything around it `.parent`. Use
  `model::joins(doc, surface, volume)` (names it, or delegated down to
  it), never `ConnectionSurface::joins()`, for a volume's inputs;
  `seam_sides()` gives the leaves that meet a surface. Composite outputs
  add children's `ContentsMeasure`s up (Volume, Mass, Centre of gravity,
  Envelope only). Tests: `tests/nested.*` (root → Housing → Left → Core
  and Cap, plus Right and Base), shared by every suite.
- **Generated children** (L4, decision 0022, `generated.hpp`): a
  composite may have a `generator` (vs_proc graph JSON) and `templates`
  (`ChildTemplate`: static, parametric or procedural contents, parameters,
  outputs, `origin`), both written to JSON only when set. A
  `GeneratedLayout` (boxes, joins on axis-aligned planes, delegations) is
  written into a copy of the document by `expand()`: datum points and
  planes in the composite's layout, children with `Volume::generated`
  appended after every authored volume, ids derived from the composite
  and the generator's keys. A copy of a parametric template carries
  `contents_offset` (box min minus `origin`): `build_contents()` rebuilds
  it with inputs moved back and moves the result. Generated volumes live
  only in evaluated documents; their names needn't be distinct.
  `model.composite.generated_link` refuses links from outside into a
  generating composite's inputs. Tests: `tests/rack.*` (Base and a Rack
  with a Cell template and a Seat), shared by the suites.

- **Configurations** (L5, decision 0037, `configuration.hpp`,
  `design_table.hpp`): `Document::configurations` give values
  (expressions) to the numbers a link may drive (root and composite
  layout variables, volume parameters; labels `width`,
  `Housing/width`, `Base.thickness`), and `active_configuration` is
  saved; both are written to JSON only when set. `configured(doc)` is
  the document as it builds (active values written in, configurations
  stripped): `build()`, `validate()`, the engine and the CLI build it;
  `build_layout()` / `build_contents()` build what they're given.
  `check()` refuses any configuration that wouldn't build
  (`model.configuration.*`), so switching never fails; the app's
  history prunes values whose target an edit removed
  (`prune_configurations()`). Design tables are the configurations as
  CSV (rows by name keep their ids; importing replaces them all).

- **Drawings** (L6, decision 0038, `drawing.hpp`): `Document::drawings`
  (written to JSON only when set) hold each drawing's sheet, views
  (direction, shown volumes, what is drawn, scale, sheet position) and
  dimensions (horizontal, vertical or diameter between `Anchor`s on the
  origin, a boundary, a connection surface or contents). Only the
  definition is saved; `layout_inputs()` strips drawings, so nothing that
  builds sees them. `check()` adds `model.drawing.*`;
  `prune_drawings()` drops what an edit removed (the app's history calls
  it after every edit). Nothing can drive a dimension.

## vs_validate essentials

- **Layout checks** (`layout.hpp`): `validate_layout(doc, build, settings)`
  takes a `model::build()` result. Overlap (`validate.overlap`: kernel
  `common`, depth = the region box's thinnest side, deeper than the
  tolerance), undeclared contact (`validate.contact` per pair of faces
  lying against each other, named from `VolumeBuild::boundary_faces`;
  `validate.contact.edge` for an edge or point touch), skipped for pairs
  sharing a bonded or bolted surface (a clearance surface doesn't excuse
  contact) or involving a keep-out, and between siblings only (L3);
  `validate.nesting.outside` is a child reaching out of its composite
  (skipped only when the parent is exactly its box). Connection rules
  (`validate.connection.*`): patch on each joined boundary (and each child
  it is delegated to), never floating inside or poking through one, clearance gap
  and flange wall at least the minimum feature size, ISO 273 holes clear of
  the patch edge and each other by the minimum wall. Candidate pairs come
  from a sweep along x; an optional `ParallelFor` (`parallel.hpp`) runs
  pairs and surfaces in fixed chunks with their own boundary copies,
  folded in order, so the report is the same on any thread count (0031).
  Pairs of boundaries that are exactly their boxes, and rectangular
  patches on them, are checked by arithmetic (`LayoutOptions::
  box_fast_path`, decision 0032): same codes, subjects and face names,
  numbers within the tolerance of OCCT's; `test_box_path.cpp` compares
  both paths on random layouts. Keep the box path and OCCT's in step.
  Pairs of boxes copy no boundary, boxes' face names are found only for
  reported contacts, box and rectangle tests read
  `face_surface_plane()`, and a rectangle within the tolerance of a box's
  side isn't sampled (`LayoutOptions::sample_every_patch` samples it):
  reports identical to the bit (decision 0034).
  `vs_validate_tests "[.scaling]"` times the layout phases on n × n grids.
- **Containment** (`containment.hpp`): `check_containment(boundary, solid
  or MeshView, volume subject, settings)`, for M9 to call after generation.
  B-rep reach is measured over tessellated points of the part outside, not
  only its vertices (H2). Meshes get their FP32 rounding as extra slack;
  non-finite vertices are `validate.undecided`. Large meshes (M12): a
  convex fast path (one lump, planar faces), otherwise octree cells
  certified by distance and the rest classified in chunks through an
  optional parallel-for, each worker with its own boundary copy and
  classifier.
- **Routing** (`registry.hpp`): every `validate.*` code is registered with
  its severity and target; `route()` sends any diagnostic to exactly one of
  layout, connection surface, feature (with the volume, inside contents),
  node or contents (`validate.undecided` by its first subject). Tests check
  every emitted `validate.*` code is registered.
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
  a lattice, constrained-Delaunay triangulated (`triangulate.hpp`, our own:
  `orient()` is exactly antisymmetric, segments are checked through a box
  tree, flips are capped; H4, decision 0026). Vertex ids derive from a
  hash of the inputs. Meshers never divide a seam themselves: they call
  `add_seam_vertices()` and `add_seam_face()`, and
  join their own faces to `Seam::loops` (with `segment_starts`).
- **Weld** (`weld.hpp`): merges vertices by seam id only, checks each volume
  has exactly the seam's triangles (`mesh.weld.nonconforming`, surface
  first; a hole fill counts, and must be whole), removes the paired seam
  faces (and paired `Seam::hole_fills`, where neither side has a bolt
  hole) and checks the result is watertight and manifold. Decision 0007
  has the reasoning.
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
  exact FP32 position (`attach_seam_vertices()`, -0 equal to +0), so baked
  volumes weld. **Files** (`io.hpp`): STL out (binary) and in, 3MF out (one
  object, or one per volume with its id) and in (our own writer and XML
  reader, zipped with miniz; prefixes, end tags and character references
  read since H4), OBJ in. Readers refuse coordinates that overflow a float
  and store -0 as +0; writers refuse a mesh `Mesh::check()` refuses
  (non-finite positions included). `import.hpp` adds a
  mesh file as a static volume boxed by its bounds.
- **Curved seams** (L2, decision 0020): a `CylinderPatch` (vs_model: a
  layout axis, radius, start angle, sweep, start and length; a full turn
  is a band) gets a seam in unrolled coordinates (`Seam::curved()`,
  `band`, `to_uv()` / `to_model()` / `offset()` / `normal_at()`,
  `triangle_uv()` across a band's cut); derived spacing keeps chords
  within `kSeamDeflection`; a band's spacing is at most a sixteenth of
  its circumference (H4). B-rep volumes conform by imprinting the patch
  (`kernel::tessellate(..., imprints)`; a band as two half turns, H4) and
  replacing its pieces with the seam; procedural ones through the same
  stitch (vs_proc), a band's two rims each to its loop. The engine gives
  triangles touching a curved
  seam a containment slack (`MeshView::centroid_slack`). Bolted flanges
  stay planar (`model.connection.curved_flange`). Planar seams, meshes
  and hashes are exactly what they were; keep it that way.
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
  without generating. The engine builds layouts from `layout_inputs()`
  and keeps the last four (with their layout checks and lazily computed
  shape hashes, `LayoutShapeHashes`), reusing an equal one or refreshing
  leaf parameters of one equal in shape, and keeps its last check
  (`EngineOptions::reuse_layouts`, on by default; decision 0029,
  `stats.layouts_reused`). The engine keeps each volume's last valid contents.
  A composite (L3) is never generated or cached: after its children (they
  are upstream of it) it takes the worst of their statuses, a key hashing
  its definition and their keys, and outputs adding their measures up.
  `combine()`, 3MF and STEP take leaves only; STEP parts of children are
  named by path ("Housing / Left / Core").
- **Generated children** (L4): when a composite has a generator,
  `evaluate()` builds the layout once after `check()`, asks the
  `LayoutGenerator` (`set_layout_generator()`; `proc::install()` sets it)
  per generating composite, caches the outcome by the composite's key,
  `model::expand()`s and re-`check()`s, then runs as L3 on the expanded
  document (`Evaluation::document` and `volumes` include the children). A
  failed generator or refused expansion fails the composite (Stale with no
  generator installed); children failing a layout check fail it too
  (`eval.generated.layout`). `stats.layouts_generated` counts runs.
- **States.** Current; Failed (generation or containment failed, or the
  boundary or a parameter: shows its last valid contents as
  `stale_contents`, blocks only its dependants, which get `eval.blocked`);
  Stale (blocked, cancelled, preview, procedural with no generator); Regenerating
  while running (`states()`). Failed results are cached too.
- **Cache** (`cache.hpp`): `cache_key()` hashes engine version, the
  `ProductSettings` (H3; the node cache's keys too), kind,
  content definition, the boundary's exact shape and named faces, joining
  surfaces, parameters, seed and measured outputs; static keys leave
  parameters and seed out. Memory always, an LRU under
  `EngineOptions::memory_cache_bytes` (2 GiB; meshes by their arrays,
  solids an estimated 256 KiB, H5); disk (`cache/<key>.brep` and
  `.json`) for clean B-rep contents, and since M17 `.mesh` for meshes;
  `encode_entry()` / `insert_files()` move entries in and out of a
  project's `cache/` (`project_cache_files()` skips parametric contents,
  whose rebuild lives in memory). `measure.hpp` measures outputs
  of meshes too (public since M12, for vs_proc). `vstudio-cli rebuild
  --cache` uses a folder project's `cache/`, or reads a zip project's
  `cache/` entries and saves them back into the zip (H4).
- **Determinism** (`thread_pool.hpp`): `run()`, `parallel_for()` and
  `parallel_reduce()` over fixed-size chunks, folded in chunk order; nested
  calls share the workers (a stack of batches; callers work on their own
  while waiting), so one volume's generation uses every thread. Catch2
  assertions aren't thread-safe: tasks record, tests check afterwards.
  `output_hashes_by_threads()` and `check_determinism` compare output hashes (BRep text, mesh, outputs).
  Pools of size 0 take `VS_THREADS` when set (`default_thread_count()`);
  CI reruns the vs_proc and vs_eval suites on 1, 4 and 16 threads (H3).
  `WorkerPriority::Low` (`EngineOptions::worker_priority`) runs workers
  below normal priority; the app does, so generation never starves the UI
  (H7, decision 0036). It changes no result.
- **Configurations** (L5): `evaluate()` checks the authored document,
  then evaluates `model::configured()` (`Evaluation::configuration`,
  `configuration_name`); keys see values only, so switching back to a
  configuration regenerates nothing.
- **Generators** (`generator.hpp`): a library above vs_eval plugs a
  content kind in with `Engine::set_generator(kind, g)`: `prepare()` on
  the calling thread, then a job on the pool that fills
  `GeneratedContents` (`extra` holds opaque contents, hashed into the
  output hash). vs_proc's is installed by `proc::install()` (M10).
- **Imports** (`import.hpp`): STEP, IGES, STL, 3MF, OBJ as a new static
  volume boxed by its bounds, or `import_contents()` into an existing one
  (then containment matters). `eval.*` codes on a volume route to its
  contents. Decision 0009.
- **Phases** (decision 0030): `ProgressEvent::Kind::PhaseFinished` names
  each step of an evaluation as it ends (`cancel.hpp`), on the calling
  thread; `vstudio-cli bench-rack [--cells N,...] [--cell mm] [--threads N]
  [--csv out]` uses it to time `preview()` on the rack fixture's shape at
  10 to 10,000 generated cells, phase by phase with peak RSS (Linux).
- **Tests** build scenes in code (`tests/scenes.*`: a row of parametric
  boxes; a chain A → B by a link, plus C) and reuse the bracket.
  `vstudio-cli rebuild <project> [--threads N] [--check-determinism]
  [--weld] [--stl out] [--3mf out [--per-volume]] [--cache]
  [--set vol.param=value] [--config name] [--twice]`; `configs <project>
  [--build] [--activate name|-]` and `design-table <project> [--all]
  [--out t.csv] | --import t.csv` (L5); `bench-edit [--volumes N]
  [--no-reuse] [--project p]` times edits outside generation (0029).

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
  meshes, unsigned for patches. Leaves are sorted and the angles come
  from `atan2_det()`, so it is the same on every standard library (H3,
  golden hashes in `test_distance_golden.cpp`). `field_layout.hpp` is
  where operations keep their numbers, shared with vs_gpu's shaders.
- **Graphs** (`graph.hpp`): parameters with defaults, nodes (id, type,
  name, `settings` for unconnected Scalar/Vector inputs and properties),
  edges output → input, exactly one `output` node. JSON round-trips
  exactly. **Checks** (`check.hpp`): `check_graph()` (`proc.graph.*`);
  `edit()` / `connect()` keep a change unless it is wrong (type mismatch,
  loop, a second output, ...), allowing unfinished graphs (`no_output`,
  `missing_input`, H3); `check_volume_graph()` adds
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
  query nodes always run), an LRU under 1 GiB sized by `value_bytes()`
  (H5).
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
  Every meshing stage takes an optional `CancellationToken*` and returns
  empty (`ContentsMesh::cancelled`) within a fraction of a second;
  `NodeContext::cancelled()` stops heavy nodes, and a stopped node or job
  is never cached (H5, decision 0027).
- **Parallel surface nets** (L1b, decision 0019): `surface_nets(samples,
  pool)` is `unmerged_net()` (vertex and polygon passes in z layers,
  each counted, then filled in place in grid order; a `MeshingBackend`
  may take it) then `merge_net()` (slabs of `kMergeSlabLayers` cube
  layers one after another, each with only its own fans, compacting in
  place behind it; inside a slab, rows of cubes in waves `j + 3k`:
  earlier neighbours always in earlier waves, rows of a wave three apart;
  decision 0028). The mesh is
  `surface_nets_reference()`'s (M11's serial code, kept in
  `src/surface_nets_reference.cpp`) bit for bit on any thread count;
  `test_parallel_mesher.cpp` compares them element by element. **Change
  the meshing in both, or the tests fail.** `clip_to_boundary()` also
  offers the clip to the backend; `closed_manifold()` is the parallel
  stand-in for `mesh::analyse()` that `mesh_contents()` runs first.
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
- **Children** (L4, decision 0022, `src/nodes_children.cpp`): port type
  `Children` (a `model::GeneratedLayout`); `children.grid` (the boundary
  box in cells, neighbours joined bonded / clearance / none, patches inset
  by `margin`), `children.at_frames` (boxes at frames), `children.merge`,
  `children.delegate` (a joining surface to the child with a face on it),
  each child from a `template` property and optionally `values` for a
  template `parameter`; `output.children` ends a composite's generator.
  `check_volume_graph()` adds `proc.graph.output_kind` (the wrong output
  for the volume) and `proc.reference.template`. `ProceduralGenerator` is
  also the engine's `LayoutGenerator`. `test_children.cpp` covers nodes,
  checks, the engine, failures and welds.
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
  CPU baseline; on Linux it prints the stages' and the engine's peak
  RSS) cover M12. `test_mvp_workflows.cpp` (`[mvp]`, M17) walks
  block out, fill, iterate, swap and ship end to end.

## vs_draw essentials (L6)

- **What.** Drawings from evaluations (decision 0038, plan
  `docs/plans/L6-drawings.md`). `draw(evaluation, drawing, options)`
  returns a `DrawingResult`: a `Sheet` (paths and texts in sheet mm,
  origin lower left, each with a `Pen`), every dimension's value and
  text, and `draw.*` diagnostics. Nothing is stored: a drawing is
  computed from each evaluation, so disposable dimensions follow their
  contents.
- **Anchors.** On the origin, a boundary or a connection surface:
  stable (missing is `draw.dimension.unresolved`, an error). On contents:
  disposable, written in parentheses in their own pen, and
  `draw.dimension.lost` (a warning) when gone. Extremes are exact for
  B-reps (`kernel::extent()`), vertices' for meshes; a composite's
  contents are its leaves'. Faces must face along the dimension, circles
  the viewer (`draw.dimension.not_parallel`, `.not_circle`).
- **Views** (`view.hpp`): `ViewProjector` draws boundaries and patches
  whole as outlines, and contents with our own hidden-line removal
  (B-rep edges and silhouettes from `kernel::tessellate()`, mesh borders,
  creases and silhouettes; a ray test through a uniform grid; sampled,
  split, simplified). FP64, fixed order, threads folded in order: the
  same files on any thread count. Edge-on facets neither hide nor count
  as front faces.
- **Files** (`sheet.hpp`): our own SVG, PDF 1.4 and DXF R12 writers,
  fixed precision; `write_sheet()` by extension (`draw.file.*`).
  `standard_drawing()` lays out front, top, right and isometric views
  third-angle with overall dimensions.
- **CLI**: `vstudio-cli drawing <project> [<name>] [--standard [<name>]]
  [--save] [--svg out] [--pdf out] [--dxf out] [--config name]
  [--threads N]`.

## vs_gpu essentials (L1, L1b)

- **What.** Procedural fields sampled on the GPU with exactly the CPU's
  values (decision 0018, plan `docs/plans/L1-gpu-field-evaluation.md`).
  `gpu::install()` opens an OpenGL 4.3 compute context (WGL; other
  platforms say why not), runs `self_check()` (21 fields, every value
  compared) and installs itself as `proc::set_sampling_backend()`;
  `proc::sample()` then sends grids of 32k+ samples to it, `sample_cpu()`
  is the CPU alone. `sample_gpu()` forces the GPU (tests, benchmarks).
- **How.** `src/glsl.cpp` writes one GLSL function per node, numbers in a
  storage buffer (shaders depend on a tree's shape only), constants as bit
  patterns, every double `precise`, `sqrt_rn()` for correctly rounded
  roots, `std::min`/`max`/`clamp` semantics, the boundary BVH walked in
  the CPU's order. `src/device.cpp` is the context's thread; GL entry
  points are loaded at run time (`src/gl.hpp`), so no GL SDK is needed.
  **Any change to `FieldExpr::eval()`, `TriangleDistance` or the
  infill maths must be mirrored in `glsl.cpp`**; `vs_gpu_tests` catches
  a mismatch on a machine with a GPU.
- **Meshing (L1b, decision 0019).** `src/meshing.cpp`: `clip_gpu()` (the
  boundary distance through the field shader) and `unmerged_net_gpu()`
  (four passes: tetrahedron masks, vertices, triangle counts, triangles;
  outputs placed by exclusive sums in grid order), equal to
  `unmerged_net_cpu()` element for element; `meshing_self_check()`
  guards them. **Any change to `unmerged_net_cpu()` must be mirrored
  there.** NVIDIA's compiler loses exactness on local arrays of doubles
  indexed at run time: recompute instead (decision 0019).
- **Calibration.** After the checks, `install()` times sampling, clip and
  unmerged net on a half-million-sample grid on the GPU and on every CPU
  thread, and gives the GPU only the stages it won (`Status::sampling`,
  `.clip`, `.meshing`, `.calibration`); about 3 s on the RTX 2070 Super
  desktop. `InstallOptions::force` (tests, `--gpu=force`) skips it.
- **Where.** The app installs it at start on a background thread (`--cpu`
  skips; About says which stage runs where); `vstudio-cli --gpu <command>`
  or `--gpu=force <command>`, installed only for `rebuild`, `inspect`,
  `bake` and `bench-gyroid` (H4). `vstudio-cli selftest` says whether it
  is available.
- **Threads and shutdown (H6, decision 0035).** `Device::run()` shares a
  heap waiter with its job, rethrows the job's exceptions and returns
  false once the device stops; programs are capped at 64 (LRU).
  `shared_device()` hands out a `shared_ptr` held per call;
  `gpu::shutdown()` drops the process's reference (the last caller closes
  it), and the app and the CLI call it before exiting. The state is never
  destroyed, so nothing GL runs in static destructors. Every GL error
  fails the call (`gl_errors()`), so the CPU takes over; buffers respect
  `GL_MAX_SHADER_STORAGE_BLOCK_SIZE`; field shaders are dispatched in 2D
  (`dispatch_2d()`); the context is a 4.3 core one on a message-only
  window where the driver allows it.

## vs_app essentials

- **Build** with the `windows-msvc-app` preset only (Qt 6 from the vcpkg
  `app` feature; its first build compiles qtbase twice, for the host tools
  and the target). CI doesn't build the app. The Qt-free files
  (`authoring`, `configurations`, `dimension_drag`, `drawings`, `exporter`, `graph_edit`, `guidance`,
  `history`, `icons`, `node_preview`, `outline`, `picking`, `reference_choices`,
  `scene`, `ship`, `sketch_editor`, `tool_catalog`) are `vs_app_core`, built on every
  platform and tested by `vs_app_core_tests` on every CI job (plan H1);
  keep Qt and OCCT's visualisation out of them. The rest but `main.cpp` is
  `vs_app_ui`; `vs_app_tests` drives it (`tests/wait.*` waits on Qt's
  event loop; `tests/samples.*` is shared and Qt-free). Tests tagged
  `[gui]` open windows and render with OpenGL, so they need a desktop
  session and a GPU.
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
  whole project snapshots (blobs shared, at most 200 undo steps, H5), so undo restores identical hashes and refuses
  changes that add a `model::check()` error; the current snapshot's
  findings are kept, so an edit checks only the new document (0029). `reference_choices.hpp` lists
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
- **Interactive regeneration** (M16, decision 0016): `dimension_drag.*`
  (no Qt) gives a selected sketch's or extrude's handles and the Command a
  drag makes (a bare variable name drags the variable); the viewport draws
  and hit-tests them (`set_handles()`, `handleDragged`), and
  `MainWindow::begin_drag()` / `drag_to()` / `end_drag()` preview through
  `Session::drag_preview()` (preview tier only, stale contents left out)
  and commit one step. Sketch mode drags dimension labels
  (`SketchEditor::drag_dimension()`). Procedural jobs honour cancellation
  (`GeneratedContents::cancelled`). `swap_contents()` (`authoring.hpp`)
  swaps one volume between placeholder, modelled, procedural and baked;
  `guidance.*` (no Qt) is the habit guidance `MainWindow::guide()` shows.
- **Picking** (M16): B-rep display meshes keep `faces` (triangle ranges
  with kernel faces) and `edge_faces`; `picking.*` (no Qt) names a picked
  part by surface against `reference_choices()` and lists what may be
  picked (`pickable_items()`, edit-in-context rules). `Viewport::
  set_pick_mode()`, `pick_at()` (clicking again in place picks what's
  behind), `project()` and `look_along()`; feature dialogs' Pick buttons
  (`FeatureDialog::begin_pick()`, `apply_pick()`).
- **Composites** (L3, decision 0021): the volume tree nests children
  under their composite (`VolumeRow::parent`, `depth`); swap offers
  Composite; edit in context on a composite edits its layout and makes it
  `MainWindow::authoring_level()`, where new volumes, surfaces, keep-outs
  and links go (their dialogs take the level and list only its layout's
  features and volumes; descendants stay undimmed through
  `Viewport::set_context(..., inside)`). `set_delegation()` /
  `MainWindow::set_met_by()` is the surface menu's "Met inside ... by";
  deleting a composite deletes everything inside it.
- **Generated children** (L4, decision 0022): rows from the evaluated
  document, so generated children nest under their composite
  (`VolumeRow::generated`, "Generated Parametric"); they are read-only
  (`MainWindow::is_generated()`, edit in context refused; their menu opens
  the composite's generator). `set_graph()` / `graph_of()` work on a
  composite's generator (`starter_generator()`, Generate children / Edit
  generator / Stop generating children), the node form lists the
  composite's templates, a Children value previews as boxes, and
  `make_template()` / `MainWindow::make_template_of()` turns an authored
  child into a template (keeping its id; origin from its built box).
- **Shipping** (M17, decision 0017): `ship.*` (no Qt) is
  `check_for_export()`: `validate()`, the shown evaluation's volume
  diagnostics, volumes not current, and the weld; Validate (Ctrl+R) and every
  export run it first (errors stop, warnings ask). A weld error stops
  STL and 3MF but not STEP (`ok_for(ShipTarget)`), and export reuses
  the check's weld (`ShipCheck::combined`, H5). `MainWindow::validate()`,
  `export_step_to()`, `import_file()` (a new static volume) and
  `save_to()` (with the shown evaluation's caches while File > Save Caches
  With Project is on) are the entry points tests drive; `Session::open()`
  loads a project's caches into the engine. The GUI walk of the five
  workflows is `docs/manual-tests/mvp-workflows.md`.
- **H7** (decision 0036): `Session` drops whatever a newer request replaced
  (drag previews wait in one slot, progress is the newest request's,
  superseded evaluations build no scene, node previews are cancelled by a
  token) and clears its caches on a new project. The viewport redraws in
  full only when what it shows changes (`refresh_all()`; hover draws the
  immediate layer) and keeps big B-reps' faces pickable across parts.
  New, Open and Close ask about unsaved changes
  (`MainWindow::set_unsaved_prompt()` for tests); sketch mode owns Ctrl+Z,
  Ctrl+Y, Esc and F. GDI screen grabs can't see the OpenGL viewport on the
  desktop's NVIDIA driver, so `[gui]` tests compare off-screen renders
  and say when a grab saw nothing.
- **Configurations** (L5, decision 0037): `configurations.*` (no Qt)
  holds the commands (add, copy, rename, remove, set a value, show one,
  set the design table) and what the toolbar, the design table dialog
  and the inspector show (`configuration_choices()`, `active_value()`,
  `table_for_editing()`, `addable_columns()`). In the window: the
  tab row's Configuration box (`MainWindow::configuration_box()`,
  `show_configuration()`; one undo step, follows the history), the
  Design Table… (`design_table_dialog.*`, `open_design_table()`: a grid
  of configurations by driven numbers, add / copy / remove rows, add
  columns, CSV import and export; OK is one `set_design_table()` step, a
  refused table keeps the dialog open with the reason), and the
  inspector and Variables panel showing a driven number as `<value>
  (configuration: <expr>)`. Tests: `test_configurations_ui.cpp`.
- **Drawings** (L6, decision 0038): `drawings.*` (no Qt) holds the
  commands (add, rename, remove drawings; add, change, remove views and
  dimensions; move a dimension's line) and what the Browser and dialogs
  list (`drawing_rows()`, `anchor_choices()`: stable anchors first,
  disposable ones in parentheses). In the window: the Browser's Drawings
  section, the drawing page in place of the viewport (`drawing_window.*`,
  `SheetView` paints the `Sheet` with QPainter, redrawn on the session's
  worker after every shown evaluation and history change), modeless
  `ViewDialog` and `DimensionDialog`, the Drawing toolbar's New (a
  `standard_drawing()` of the shown evaluation) and File > Export
  Drawing… (SVG, PDF, DXF by extension, after the shipping check as
  for STEP). Entry points: `new_drawing()`, `open_drawing()`,
  `delete_drawing_item()`, `rename_drawing_item()`,
  `add_drawing_view()`, `add_drawing_dimension()`,
  `export_drawing_to()`. Tests: `test_drawings.cpp` (core),
  `test_drawings_ui.cpp`.
- **Context toolbar** (U1, decision 0039): `tool_catalog.*`
  (no Qt) is every tool as a catalogue row (name, label, tooltip,
  shortcut, icon), each context's groups (Layout, Contents, Sketch, Graph,
  Drawing), `current_context()` derived from what the window has open,
  and `tab_availability()` / `availability()` with a reason when
  something can't be used; `icons.*` (no Qt) is every icon as strokes on
  a 24-unit grid. In the window (phase 2): `actions.*` makes one
  `QAction` per tool (object name `tool_<name>`, added to the window so
  shortcuts work from any toolbar), `icon_painter.*` paints the icons,
  and `context_bar.*` is the tab row (with the configuration box, Design
  Table… and Regenerate on its right) over one toolbar per context.
  `MainWindow::refresh_tools()` follows what is open and selected;
  `enter_context()` is a tab click, `run_tool()` a button. The menus keep
  File, Edit, View and Help. Sketch mode (phase 3) is only the canvas:
  the Sketch tab drives it, its status and constraint list
  (`SketchMode::side_panel()`) sit in the Sketch dock on the right, and
  the Browser folds while sketching. Phase 4: `browser.*` is the left
  panel, one `Browser` (a `FoldPanel`) with Volumes, Features, Variables
  and Drawings sections that fold to a line (`MainWindow::browser()`, in
  the dock `drawings_dock()` returns); the Browser and the Inspector
  (`inspector_panel()`) fold to a strip and the viewport takes the room.
  Phase 5: `problems.*` is the status bar's count (`problems_button()`)
  and the Problems drawer under the viewport (`problems_drawer()`, in a
  splitter, holding `diagnostics_list()`); it opens itself only when
  something the person started reports problems (`show_diagnostics(...,
  started)`), never for a background evaluation; the Graph dock follows
  the Graph context. Phase 6: the drawing is a page of the central stack
  (the Drawing tab shows it in place of the viewport); `theme.*` is one
  palette and style sheet on the main window (colours and borders only);
  `save_layout()` / `restore_layout()` keep the panels between runs
  (`set_remember_layout()`, off in tests and captures). Plan:
  `docs/plans/U1-context-toolbar.md`. Tests: `test_tool_catalog.cpp`,
  `test_context_bar_ui.cpp`, `test_browser_ui.cpp`, `test_problems_ui.cpp`.
- **Running it**: `build/windows-msvc-app/apps/vs_app/vs_app.exe [project]
  [--select <name>] [--view iso|top|front|...] [--context <volume>]
  [--sketch <sketch>] [--edit <feature>] [--graph <volume> [--node <node>]
  [--slice]] [--drawing <name> [--drawing-image out.bmp]] [--size WxH]
  [--screenshot out.bmp] [--viewport-image out.bmp]`; the capture flags
  wait for the evaluation, save (BMP: this Qt has no PNG) and quit.
- **From a Claude desktop-app session** `%LOCALAPPDATA%` is virtualised and
  Qt's configure mistakes it for a symlink: configure once with
  `-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=C:/vsbt` (decision 0013).

## Code style

- C++20, warnings as errors. `#pragma once`. Namespaces `vs::<short>`.
- Types `PascalCase`, functions and variables `snake_case`, constants `kPascalCase`.
- Public headers document intent and invariants; keep implementation detail in `src/`.

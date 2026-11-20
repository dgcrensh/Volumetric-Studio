# H — Hardening (plan)

Nov 20, 2026 · follows L4 · precedes L5 (configurations and design tables)

**Status: planned, revised.** The first draft (`1e2a378`) also moved procedural fields and the unmerged surface net to FP32 for the GPU. Review cut that (see "What was cut" below) and reordered the phases so that everything a Linux cloud session can build and test runs first, and the work that needs the Windows desktop (a GPU, the Qt app, `[gui]` suites) is grouped at the end. Dallas accepted the revision on Nov 20, 2026.

Source line numbers below come from an audit of the pre-L4 baseline (`827470d`) and may have shifted with L4 (`ddc4ced`).

## Decisions and scope

1. **No FP32 in this pass.** Fields, the boundary distance and the unmerged net stay FP64, and CPU and GPU stay equal bit for bit (L1, L1b). Golden hashes change only where a determinism fix requires it (item 3).
2. **Verify, then fix.** The item lists come from an automated audit. Every item starts with a regression test that fails on `main`; an item that can't be reproduced is dropped and listed as such in its PR. Spot checks during review confirmed four items (the `Device::run()` use-after-free, `atan2` in `corner_angle()`, `nth_element` leaf order in the BVH, and `eval.nondeterministic` not failing the volume).
3. **One engine version bump.** `core::kEngineVersion` goes from `1` to `2` once, in H3, for the BVH leaf order, the `atan2` replacement and `ProductSettings` in cache keys. Saved `.vsproj` files still load; `engine: 1` caches miss cleanly and regenerate.
4. **Input hardening scope.** Fix real crash, UB and parser-correctness defects in the file loaders (`read_obj`, `read_3mf`, `read_stl`, `project.cpp`, `serialize.cpp`). Adversarial DoS hardening (zip/3MF quotas, JSON/expression depth limits, path sandboxing) stays out of scope for this personal tool.
5. **Linux first, Windows grouped.** H1 to H5 are built and tested on Linux (cloud sessions, Linux CI); CI's Windows MSVC job still checks that every library compiles. H6 and H7 need the desktop and run together there. Changes made on Linux to code only the desktop can run (the app's CMake, GLSL) are kept minimal and are verified first thing in H6.
6. **Sanitizers: ASan and UBSan only.** TSan is dropped: OCCT and TBB from vcpkg aren't instrumented, so it reports noise. Thread safety stays covered by the determinism tests across thread counts.
7. **Branches.** One branch and PR against `main` per phase: `h1-ci-and-core-safety`, `h2-design-rule-bypasses`, `h3-determinism-and-cache-keys`, `h4-mesh-io-and-cli`, `h5-memory-and-cancellation`, then on the desktop `h6-gpu-fixes` and `h7-app-fixes`. Splitting the large files (`MainWindow`, `rebuild.cpp`, `mesher.cpp`) is deferred until the fixes land.

## Sequencing

```
Linux (cloud sessions)                                          Windows desktop
H1 CI & core safety ──> H2 design-rule bypasses ──> H3 determinism & cache keys (v2)
                    └─> H4 mesh, file I/O & CLI ──────────────┴─> H5 memory & cancellation ──> L5 (libraries)
                                                                                  H6 vs_gpu ──> H7 vs_app
```

- **H1 to H5 come before L5.** L5 leans on exactly what they fix: incremental rebuild reuse and dependency edges (H2), cache keys that include settings (H3), and memory that grows with every variant (H5).
- **H6 and H7** run in one stretch on the desktop, either before L5 or together with L5's app work, since both touch `MainWindow`. Until then the Windows app build is unverified for H1's CMake split; `gpu::install()`'s self-check keeps the GPU off if anything in H3 or H6's field maths disagrees with the CPU.

| Phase | Where | Branch | Scope | Verification |
| --- | --- | --- | --- | --- |
| **H1** | Linux | `h1-ci-and-core-safety` | `vs_app_core` split, ASan+UBSan CI, OCCT/PlaneGCS/eval/mesher UB and leak fixes | Linux CI (GCC, Clang, ASan+UBSan), Windows MSVC CI |
| **H2** | Linux | `h2-design-rule-bypasses` | Containment, encapsulation, one-way flow, profile and rebuild bypasses | Linux CI, Windows MSVC CI |
| **H3** | Linux | `h3-determinism-and-cache-keys` | BVH leaf order, `atan2`, settings in cache keys, graph output codes, thin-grid merge memory, `kEngineVersion = 2` | Linux CI on 1, 4 and 16 threads, Windows MSVC CI |
| **H4** | Linux | `h4-mesh-io-and-cli` | CDT robustness, L2 band seams and tessellation, OBJ/3MF/STL/`.vsproj` fixes, CLI fixes | Linux CI, Windows MSVC CI |
| **H5** | Linux | `h5-memory-and-cancellation` | Shared blobs, bounded caches, library cancellation, peak RAM and CPU wins, Qt-free app fixes | Linux CI incl. `vs_app_core_tests`, `bench-gyroid` on Linux |
| **H6** | Windows | `h6-gpu-fixes` | `vs_gpu` concurrency and GL safety, verifying H1's app split | `vs_gpu_tests`, `bench-gyroid --gpu`, `windows-msvc-app` build |
| **H7** | Windows | `h7-app-fixes` | Qt app fixes, `Session` races, the two flaky app tests | `vs_app_tests` incl. `[gui]`, repeated runs of the flaky tests |

---

## Phase H1 — CI, sanitizers and core safety (Linux)

**Goal:** Remove the highest-risk lifetime, leak and immutability hazards and turn on sanitizers and headless app-logic tests before touching geometry.

### Build and CI
- **Split `vs_app_core` from `vs_app_ui`** (`apps/vs_app/CMakeLists.txt`): move the 13 Qt-free files (`authoring`, `dimension_drag`, `exporter`, `graph_edit`, `guidance`, `history`, `node_preview`, `outline`, `picking`, `reference_choices`, `scene`, `ship`, `sketch_editor`) into `vs_app_core`, built always (also with `VS_BUILD_APP=OFF`) and tested on every CI job by `vs_app_core_tests`. The Qt and OCCT-viz files stay in `vs_app_ui` behind `VS_BUILD_APP=ON`, which only links `vs_app_core` in addition; the `windows-msvc-app` build is checked at the start of H6. Move the tests of those files that need no Qt into `vs_app_core_tests`.
- **ASan + UBSan CI job** (Clang, `.github/workflows/ci.yml`) running every library suite and `vs_app_core_tests`. No TSan (decision 6).
- Move `permissions: contents: write` off the matrix `build` job into a `publish-cache` job that runs only on `main` / `workflow_dispatch`.
- **Build hygiene:** a `check_public_headers.cmake` for `vs_proc` (Manifold stays private, as for `vs_kernel` and `vs_sketch`); tighten `PUBLIC_LINKS` in `libs/vs_proc/CMakeLists.txt` and `libs/vs_gpu/CMakeLists.txt`; a shared `vs_model_test_fixtures` target so `../vs_model/tests/{curved,nested}.cpp` aren't recompiled into 5+ test binaries.

### Core safety and UB
- **`vs_kernel`:**
  - `op.SetNonDestructive(true)` in `run_boolean()` and `common_faces()` (`libs/vs_kernel/src/operations.cpp:85-89, 143-147`) so shared `Shape` handles are never changed in place.
  - An RAII guard closing the XCAF document (`XCAFApp_Application::GetApplication()->Close(doc)`) on every exit path of `write_step()` and `read_step_parts()` (`exchange.cpp:134-138, 166, 210`).
  - Null `poly->Parameters()` dereference (`tessellate.cpp:163, 251`); `.is_empty()` checks and `detail::guarded` in `edge_point`, `edge_uv`, `face_point` (`tessellate.cpp:336-362`), `queries.cpp:127-165` and `check.cpp:95-117`; reject `NaN` in `edge_parameter()`, `classify()` and `distance_to()`.
- **`vs_sketch` (PlaneGCS):**
  - The 5-argument `sys_.addConstraintP2PAngle(l.p1, l.p2, value, 0.0, tag)` (`solver.cpp:265`), since the 4-argument one ignores the tag (`third_party/planegcs/src/GCS.cpp:649-652`).
  - Run `all_hold()` unconditionally after `applySolution()` (`solver.cpp:402-418`), failing on `NaN`; check `*c.rad > 0` and finiteness before `write_back()` (`solver.cpp:338-341`).
- **`vs_eval`:** `eval.nondeterministic` sets `VolumeStatus::Failed` (`engine.cpp`, the determinism check), so nondeterministic contents never become current, replace `last_valid_`, unblock dependants or reach the disk cache; guard the second determinism pass against cancellation.
- **`vs_proc`:**
  - Hoist `MergeWorker` / `workers` above `merger.finish()` in `merge_net()` (`mesher.cpp:567-630`) so `Fans` spans never point into freed arena blocks.
  - Fix the inverted sign fallback in `Builder::gradient()` (`mesher.cpp:1309-1319`) and scale `Builder::contain()`'s push slack (`mesher.cpp:1183-1255`) above half an FP32 ULP at the vertex.

### Acceptance (H1)
- Linux GCC, Clang, ASan+UBSan and Windows MSVC CI pass, each running `vs_app_core_tests`.
- Regression tests: repeated `write_step` / `read_step_parts` without leaked documents, one-line `Angle` conflict reporting and post-solve redundant violation, a deliberately nondeterministic `ContentsGenerator` ending `Failed` and not disk-cached, `Builder::gradient()` falling back inward on symmetric thin walls, booleans leaving their inputs' hashes unchanged.

---

## Phase H2 — The design-rule bypasses (Linux)

**Goal:** Close every verified bypass of containment, encapsulation, one-way data flow and structural validity, across flat, curved (L2) and composite (L3) models.

### Rule 5: containment and layout validation (`vs_validate`)
- **Curved B-rep containment** (`containment.cpp:61-87`): when `mass->volume > 0`, measure reach over tessellated surface and edge points of `*outside`, not only topological vertices, so bulging spheres, cylinders, domes and fillets can't pass with `reach == 0`. A failing `mass_properties()` is `validate.undecided`.
- **Mesh containment** (`containment.cpp:112-274`):
  - Check `mesh.indices` before returning early on empty `positions`; non-finite `positions` or `centroid_slack` are `validate.undecided`.
  - Copy `boundary` (`BRepBuilderAPI_Copy`) per worker so parallel chunks never share a mutable `TopoDS_Shape`; one `SolidClassifier` per worker, not per 2,048-sample chunk; make `SolidClassifier` non-copyable.
  - Fix the octree stop condition (`containment.cpp:144`: compare against the group size saved before `group.clear()`), and stop the face-distance loop as soon as `d < r`.
- **L3 nesting and connection checks** (`layout.cpp:153-180, 421-445`):
  - Replace the dimensionally wrong `tol_ * parent->box.volume()` heuristic with an exact box-fill check (six axis-aligned planar faces matching `parent->box`), classified once per parent.
  - `validate.connection.off_boundary` also fails patches poking through or floating inside a solid; clearance gap bounds apply to `Clearance` surfaces without exempting zero-distance contact on unrelated faces.
  - Route `validate.undecided` by its first subject, and handle `PublishedOutput` and `ConnectionSurface` in `level_of_subject()` (`registry.cpp:56-77`).

### Rules 1 and 6: encapsulation, identity and the dependency graph (`vs_model`, `vs_feature`)
- **Ids:** reject `Id::null()` in `read_id()` for authored objects (`vs_model`, `vs_feature`, `vs_sketch`) and in `Checker::unique_ids()` (`check.cpp:69-88`); one shared tree-id walker in place of the four that drifted (`tree_holds`, `layout_holds`, `note_tree`, `in_tree`), including sketch constraints and `sketch.id()`; catch self-parenting composites (`v.parent == v.id`) in `volume_structure()` (`check.cpp:173-188`).
- **`DependencyGraph`** (`dependency.cpp:151-221`): chain body-changing features in `tree_edges()` so `ThroughAll` extrudes, cuts, shells, fillets, chamfers and patterns depend on every earlier body feature; resolve `VolumeBoundary` and `ConnectionSurface` nodes in `uses_layout()` as `tree_edges()` does, closing L3 cycle bypasses and stale wave scheduling.
- **Rebuild and build integrity:**
  - `vs_feature::rebuild()` (`rebuild.cpp:1545-1572`): reuse `prev` only when upstream features, sketches and `ProductSettings` are unchanged; `ExternalInputs` no longer shadow failed tree variables.
  - `vs_model/src/build.cpp`: keep a failed composite's `contents_rebuild` through `apply_contents()` (`L706-709, 849-855`); fail target boundaries when a keep-out cutter failed (`L449-454`); leave failed parametric solids out of composite measures (`L935`).
- **Profiles** (`vs_sketch/src/profile.cpp`): per-loop 2D boxes (fixing `O(L²P²)`), segment-segment intersection checks (crossing `+` rectangles, bowties, coincident circles), deflection-driven sampling of large arcs.

### Acceptance (H2)
- A regression test per bypass: bulging sphere and cylinder containment, a `NaN` mesh vertex, a 10 mm notch on a 1,000 mm composite, a null-id volume, a self-parenting composite, a `ThroughAll` feedback cycle, L3 delegated-surface wave order, a crossing `+` profile, an edited-upstream incremental rebuild.

---

## Phase H3 — Determinism and cache keys, `kEngineVersion = 2` (Linux)

**Goal:** Remove the cross-platform nondeterminism left in the boundary distance, make cache keys complete, and bump the engine version once. Everything here is CPU-side and FP64.

- **Version:** `core::kEngineVersion` (`libs/vs_core/include/vs/core/version.hpp:16`) `1 → 2`, with the golden values that change recorded in the PR.
- **Cache keys:** hash `ProductSettings` into `eval::cache_key()` (`libs/vs_eval/src/cache.cpp:50-108`) and `inputs.settings` into `proc::evaluate_graph()`'s `NodeCache` key (`libs/vs_proc/src/evaluate.cpp:190-206`). L5 needs both.
- **`TriangleDistance`** (`libs/vs_proc/src/distance.cpp`):
  - Sort each BVH leaf's `order_` range (`distance.cpp:152`) so leaf contents don't depend on the standard library's `nth_element`. The GPU walks the leaf order the CPU uploads, so it follows without a shader change.
  - Replace `std::atan2` in `corner_angle()` (`distance.cpp:72-76`) with arithmetic only (the project's no-libm rule); pseudonormals are computed on the CPU and uploaded, so the GPU follows.
  - Guard `0.0 / 0.0` in `closest_on_triangle()` (`distance.cpp:47-68`); if the guard changes a value the shader also computes, mirror it in `glsl.cpp` (checked in H6).
- **Graph checks:** split `proc.graph.output` into `proc.graph.no_output` (unfinished) and `proc.graph.multiple_outputs` (refused by `edit()`, `check.cpp:146-148`).
- **Curved surfaces in nodes:** fix normal handling of curved connection surfaces in `scatter.surface` and `input.surface` (`nodes_frames.cpp:261`, `nodes.cpp:200`).
- **Thin-grid merge memory** (`mesher.cpp:576`): give the wave strides `{1, 3, 9}` to the axes by length, longest first (or sparse wave buckets), keeping the mesh equal to `surface_nets_reference()` bit for bit.
- **Shared layout contract:** share field-buffer offsets, `LatticeCell` / `TpmsSurface` ordinals and `kMaxBvhDepth` between `vs_proc` and `vs_gpu` with `static_assert`s (they compile on Linux).

### Acceptance (H3)
- `vs_proc` and `vs_eval` suites pass identically on 1, 4 and 16 threads on Linux; a new golden-hash test of boundary-distance samples (including parts near ±1,000 mm) passes unchanged on GCC, Clang and MSVC CI, so all three standard libraries agree.
- Changing `ProductSettings` misses the cache; the golden-hash changes are listed in the PR.

---

## Phase H4 — Mesh geometry, file I/O and CLI (Linux)

**Goal:** Fix algorithmic failure modes in the CDT and L2 curved seams, real crash, UB and parser bugs in OBJ, 3MF, STL and `.vsproj` I/O, and the CLI gaps.

### Custom CDT (`libs/vs_mesh/src/triangulate.cpp`)
- Symmetrise `orient_eps(a, b, c)` across all three edge lengths (or adopt Shewchuk's adaptive `orient2d` / `incircle`), ending `Cdt::recover()` ↔ `convex_flip_ok()` deadlocks and sliver inversions.
- A deterministic flip-count cap in `Cdt::legalise()` (`L244-262`); check `constrained(x, y)` before flipping in `Cdt::recover()`; sign comparisons instead of `orient * orient < 0` (`L368-369`).
- Replace `check_segments()`'s mean-cell hash grid (`L487-526`) with an x-sorted sweep and box pruning, so mixed-scale outlines don't run out of memory or degrade to `O(M²)`.

### Curved band seams, tessellation and weld (`vs_mesh`)
- **Band seams** (`seam.cpp:311-320, 444-490`): cap the effective band spacing (or add interior column constraints) so no band triangle spans `≥ πr` or folds two diagonals onto a four-triangle cut edge.
- **Weld facing** (`weld.cpp:152-165`): judge seam triangle orientation by cyclic permutation parity against `seam.triangles` / `seam.hole_fills`, not `triangle_uv` cross products; include `hole_fills` in conformity checks; fix `"0 of N missing"` on duplicate seam triangles.
- **Conforming tessellation** (`tessellate.cpp`): periodic `±2πr` wraparound in `divide_on_seam()` box culling (`L303-335`) and the shorter arc pair on closed circular edges (`L359-378`); an interior sample in `patch_piece()` (`L445-471`) so small complement faces on nearly full cylinders aren't taken as inside the patch; orphaned Steiner points from collapsed wires (`L547-559`); de-duplicate BRepMesh interior points against curved remainder lattices (`L670-694`); propagate `make_cylinder_face` errors (`L823-827`).
- **Zero and mesh invariants:** canonicalise `-0.0f → +0.0f` in `attach_seam_vertices()` (`seam.cpp:639-642`) and `Builder::vertex()` (`io.cpp:44-51`); self-append-safe `Mesh::append(m)`; finiteness in `Mesh::check()`; shift by `p0` in `enclosed_volume()` (`mesh.cpp:79-89`) and `eval::measure_mesh()` (`measure.cpp:25-45`).

### File I/O (`vs_mesh/src/io.cpp`, `vs_model`, `vs_eval`)
- **`read_obj()`** (`io.cpp:532-539`): `k < -n` instead of `-k > n` (the `LLONG_MIN` overflow and out-of-bounds read); strip inline `#` comments.
- **`read_3mf()`** (`io.cpp:608-626`): `!tag.closing` for `vertex`, `triangle`, `component` and `item`; strip namespace prefixes (`m:model` → `model`); decode numeric character entities; keep an `Object*` instead of looking up `objects[current]` per element.
- **Finiteness:** `std::isfinite` on narrowed coordinates in the ASCII STL, OBJ and 3MF readers (so `Inf` can't pass `import_mesh_volume()`); `Mesh::check()` in `write_stl` / `write_3mf`.
- **`.vsproj` and cache files:** `fs::is_regular_file(p, ec)` in `project.cpp:44-57` (a `blobs/.` directory no longer ends in `bad_alloc`) and `ec` at lines `187, 199, 212`; check `close()` and stream state and use per-write temporary names in `project.cpp:61-83` and `cache.cpp:273-285`; check enum ranges in `cache.cpp:354-359`; check the JSON `"version"` range and member types in `serialize.cpp:604-614`.

### `vstudio-cli` (moved here from the app phase; all Linux-testable)
- `--3mf` and `--per-volume` on `rebuild` (or `export` through `eval::Engine` + `proc::install()`), so procedural projects export 3MF from the CLI; zip `.vsproj` caches in `rebuild --cache`; initialise `--gpu` only when a command needs it; integer parsing and write-error checks in `bench-gyroid --threads`; one `to_path()`; `vs_gpu` in `selftest` (reporting "not available" off Windows).

### Acceptance (H4)
- Regression tests: coarse band seam welding, band-cut edge tessellation, a 350° curved complement face, `-0.0f` STL and baked welding, `read_obj` with `LLONG_MIN` and inline comments, non-self-closing and namespace-prefixed 3MF, `blobs/.` rejection, mixed-scale CDT outlines, CLI procedural 3MF export and zip `--cache` round trip.

---

## Phase H5 — Memory, cancellation and performance (Linux)

**Goal:** Stop memory growing without bound in long sessions (L5's variants will multiply it), make heavy procedural generation cancellable inside the libraries, remove the largest hotspots, and fix the Qt-free app bugs that `vs_app_core_tests` can cover.

### Shared blobs and bounded caches
- **`model::BlobStore`** (`project.hpp:88-90`) holds `std::shared_ptr<const Blob>` (as `history.hpp:17-18` already assumes), so undo steps, preview ticks, drag frames and imports stop deep-copying blobs.
- **Byte-budget LRU** for `eval::ContentsCache::memory_` (`cache.cpp:408-465`, counting `Mesh` and `FieldContents`) and `proc::NodeCache` (`evaluate.cpp:26-30`, replacing the 4,096-entry clear); a cap on `History::undo_` depth.

### Cancellation in the libraries
- Thread `const eval::CancellationToken*` through `mesh_contents()`, `clip_to_boundary()`, `surface_nets()`, `unmerged_net()`, `merge_net()` (checked per wave), `Builder::stitch()`, `Builder::contain()` and `NodeContext` (`convert.field_to_mesh`, `scatter.*`). Wiring `Session` to it is H7.

### Peak RAM and CPU
- Limit `Builder::stitch()`'s edge and fan maps (`mesher.cpp:853-915`) to candidate and rim triangles (about 1.5–2 GB on a 10M-triangle gyroid).
- `weld()` (`weld.cpp:70-119`) takes `std::span<const Mesh* const>` (no copy in `combine.cpp:108`) and indexes only defect edges and vertices (`weld.cpp:250-264`).
- `pln.Location()` instead of `kernel::face_plane()` in `faces_of()` (`rebuild.cpp:56-59`); multi-tool `Builder::merge()` for multi-profile extrudes and revolves (`L942-948, 1000-1006`); one `build_graph()` in `VariableTable::evaluate()` (`variables.cpp:305-306`).
- Batch canonicalised floats into BLAKE3 blocks in `SampledField::hash()` (`field.cpp:479`); `format_to` and `reserve` in the 3MF and STL readers and writers.
- **The merge** is the largest remaining meshing stage (3.1 s of 10.8 s on the 100 mm gyroid, decision 0019) and stays exact; profile it here and note any further win for a later plan.

### Qt-free app fixes (`vs_app_core`)
- `sketch_editor.cpp`: signed drag clamping and degenerate-click undo steps.
- `ship.cpp`: reuse the weld instead of welding twice on STL / 3MF export; a mesh-weld error no longer blocks STEP export (`ship.cpp:42-48`).
- `History::new_errors()` compares `(code, subjects)`, not formatted messages.
- `graph_edit.cpp:199-214`: keep wire direction (`from_output`) in `connect_ports()`.
- `scene.cpp:233`: composite colour indexing (matched in `main_window.cpp:844` in H7); check `has_children` in `make_placeholder()`.

### Acceptance (H5)
- `History` shares `Blob` pointers across 50 edits; `ContentsCache` and `NodeCache` evict under byte caps; cancelling mid-`mesh_contents()` returns within 250 ms without caching partial output.
- Measured peak RSS reduction on `bench-gyroid --size 100 --cell 0.5` (Linux).
- `vs_app_core_tests` cover same-name port wiring both ways, STEP export with an unweldable mesh, composite colours and placeholders.

---

## Phase H6 — `vs_gpu` fixes (Windows desktop)

**Goal:** Fix the GPU module's concurrency and GL defects, and verify on real hardware what H1 and H3 changed without it. Fields stay FP64.

- **Verify first:** build `windows-msvc-app` (H1's split) and run `vs_gpu_tests` and `self_check()` against H3's distance changes.
- **Concurrency** (`device.cpp`, `sampler.cpp`): fix `Device::run()`'s use-after-free of the stack `done_cv` (`device.cpp:64-83`: notify under `done_mutex`, or a `std::promise`); catch exceptions in `Device::loop()`; guard `stopping_` in `run()`; lock `s.status.why` and guard concurrent `install()`.
- **Shutdown:** an explicit `gpu::shutdown()`, called before exit by `vs_app` and `vstudio-cli`, instead of destroying `Device` and calling `FreeLibrary(opengl32.dll)` during `atexit`.
- **GL safety:** `Buffer::Buffer()` out-of-bounds read for `0 < bytes < 16` (`device.cpp:127-133`); `GL_STATIC_DRAW` for read-only buffers; check `glGetError()` and `GL_MAX_SHADER_STORAGE_BLOCK_SIZE` in `sample_gpu()` and `meshing.cpp` (never silent zeros); 2D dispatches so wide flat grids stay under 65,535 workgroups; `HWND_MESSAGE` for the hidden window (`gl_context.cpp:47`) and `wglCreateContextAttribsARB` when available; cap `Device::programs_`.
- **Calibration:** `calibrate()` (`sampler.cpp:475-485`) checks `has_value()` so a failing GPU stage never wins.
- **Shaders:** make the FP64 `sqrt_rn()` subnormal-safe (`glsl.cpp:118-134`); emit `MESH{}` definitions from `meshes_.size()` (`glsl.cpp:575-582`).
- **Optional, only if reproducible:** reduce the TPMS argument to a cell before multiplying by `2π` (`tpms_distance()`) against `sin_det` overflow. Within the ±1,000 mm working space the argument stays far below overflow, so it is dropped unless a test shows a real case; if kept it changes field values on both sides and needs its own golden update.

### Acceptance (H6)
- `vs_gpu_tests` pass bit for bit (CPU == GPU) across arithmetic (including subnormals), fields, templates and meshing; `bench-gyroid --size 100 --cell 0.5 --gpu` and `--gpu=force` run clean, with calibration unchanged or better than decision 0019's table.
- `windows-msvc-app` builds and `vs_app_tests` pass on H1–H5's changes.

---

## Phase H7 — `vs_app` fixes and flaky tests (Windows desktop)

**Goal:** Fix the verified Qt lifetime, dialog, viewport and session bugs, and make the two known flaky tests reliable.

- **Flaky tests:** "UI stays responsive" and "Each kind of scene item is visible". Find the cause before changing either; the `Session` races below are the first suspects for the first. A fix is done when each passes 50 runs in a row.
- **Session** (`session.cpp`): synchronise `preparer_` (`L81, 221, 375`); coalesce `drag_preview()` into one pending slot; gate `progressed()` by generation and clear the progress UI when a drag supersedes `regenerate()`; wire `cancel_preview()` (`L115-119`) to a real `CancellationToken` for `inspect_node()` / `preview()`; bail before `finish_snapshot()` in `evaluate_job()` when superseded or cancelled; clear `generator_->cache()` and `preview_->inputs` in `set_project()` (`L251-254`).
- **Dialogs and authoring:** `FeatureDialog::complete()` (`feature_dialog.cpp:624-649`) validates only the active page (unblocking `PlaneThroughPoints`, `PlaneAxisAngle`, `PointAtRef`, `PointAxisPlane`); keep fillet / chamfer radius text on edge add or pick (`L305-329`); validate numeric fields in `ConnectionDialog`, `VolumeDialog`, `KeepOutDialog`; `end_pick()` on page switch; `MainWindow` sketch-insert double close (`main_window.cpp:1278-1282, 1627`); `QPointer<EditDialog>` and a null `screen()` check in `main.cpp:110-174`; an unsaved-changes prompt on Close, New and Open.
- **Viewport:** picking on B-reps over 60k triangles (`viewport.cpp:121-131`: face ranges and edge lines per chunk); clear `last_pick_valid` in `show_scene()`; reject behind-camera handle hits in `project()`; no full `view->Invalidate()` on hover repaints.
- **Node editor and sketch mode:** parent the `QMenu` in `GraphScene::contextMenuEvent()` (`node_editor.cpp:495`); disconnect old `NodeForm` widgets before `deleteLater()` (`L1265-1279`); pass wire direction through `node_editor.cpp:463-467`; suspend `MainWindow`'s Ctrl+Z / Ctrl+Y / Esc / F shortcuts in sketch mode; `refresh()` no longer overwrites `set_dimension_text()` errors (`sketch_mode.cpp:586-587`); composite colour in `main_window.cpp:844`.

### Acceptance (H7)
- `vs_app_tests` pass including `[gui]`, with no regressions; the two flaky tests pass 50 consecutive runs.
- Tests for datum plane and datum point authoring through `FeatureDialog`, unsaved-changes prompts, large-B-rep picking.

---

## What was cut

**Canonical FP32 fields and unmerged surface nets.** The first draft moved `FieldExpr` evaluation, BVH sampling and the unmerged net to FP32 so consumer GPUs would win every stage. Review cut it from this pass:

- **The payoff is small.** On the 100 mm gyroid (decision 0019) the engine takes 10.8 s, of which sampling, clip and the unmerged net (the stages FP32 would speed up) are about 3.3 s. The merge, containment and stitching stay on the CPU, so even an infinitely fast GPU saves at most about 30%.
- **The exactness risk is large.** L1 and L1b are CPU == GPU bit for bit. In FP32, GLSL division isn't correctly rounded (2.5 ULP allowed) and drivers may flush subnormals to zero; `precise` only stops contraction. The draft handled `sqrt` but not these, so its bit-for-bit acceptance criterion was at real risk.
- **What would bring it back:** a separate plan once a non-Windows GPU backend (Vulkan, Metal, EGL) is actually in scope, gated on a prototype that shows a worthwhile end-to-end engine win on `bench-gyroid` and exact CPU == GPU results including division and subnormals.

**TSan** (decision 6).

## Deferred follow-ups (after H7)

- **Splitting large files:** `ProjectIoController` and `DialogCoordinator` out of `main_window.cpp` (2,061 lines); split `rebuild.cpp` (1,583), `mesher.cpp` (1,456), `node_editor.cpp` (1,472) and `tessellate.cpp` (841) into focused translation units.
- **Topological naming:** expose OCCT `Generated` / `Modified` face history from `vs_kernel` so `FaceRef` no longer relies only on unbounded plane and cylinder matching.
- **Cross-platform UI and GPU backends:** port `vs_app_ui`'s `Viewport` (`Xw_Window` / `Cocoa_Window`) and add EGL / Vulkan / Metal backends to `vs_gpu`; FP32 fields would be reconsidered there (see "What was cut").

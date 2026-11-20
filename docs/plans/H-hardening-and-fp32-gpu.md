# H — Hardening and FP32 GPU fields (plan)

Nov 20, 2026 · follows L4 · decision record (for H3): `docs/decisions/0023-fp32-procedural-fields.md` (to be written on landing H3)

**Status: planned.** Implementation is deferred until development runs on the Windows desktop machine so every phase (including `vs_gpu` shader execution and `vs_app` `[gui]` suites) is verified end-to-end before merging. Source line numbers below reference the pre-L4 audit baseline (`827470d`) and may be shifted slightly by L4 (`ddc4ced`).

## Decisions and scope

1. **Canonical FP32 for procedural fields and unmerged surface nets (H3):** Move `FieldExpr` evaluation, local-recentered BVH sampling, and `UnmergedNet` construction (`vs_proc` + `vs_gpu`) to canonical IEEE-754 FP32 (keeping `-ffp-contract=off` / `/fp:precise`). Authored geometry (B-reps, sketches, layouts, boundaries, connection surfaces, seams, overlap/nesting, and CPU `Builder::contain()`) stays FP64. This lets consumer GPUs beat the CPU on every stage and removes the FP64 blocker toward future macOS/Linux GPU backends (Vulkan/Metal/EGL).
2. **Platform trajectory:** The project aspires toward macOS and Linux, though native desktop/GPU support on them is not in scope yet. Phase H1 splits Qt-free app logic (`vs_app_core`) from `vs_app_ui` so Linux CI builds and tests headless app logic now; Phase H3's FP32 move is the GPU step in that direction.
3. **Engine version bump:** Bump `core::kEngineVersion` (`1 → 2`) once in H3 alongside the FP32 migration, deterministic BVH leaf sorting, deterministic `atan2` replacement, and adding `ProductSettings` to cache keys. Saved `.vsproj` files remain loadable; older `engine: 1` disk/project caches miss cleanly and regenerate.
4. **Input hardening scope:** Fix real crash, UB, and parser-correctness defects in file loaders (`read_obj`, `read_3mf`, `read_stl`, `project.cpp`, `serialize.cpp`). Adversarial DoS hardening (ZIP/3MF size and triangle quotas, JSON/expression depth limits, symlink/path-traversal sandboxing) is explicitly out of scope for this personal tool.
5. **Workflow and sequencing:** One branch and PR against `main` per phase (`h1-ci-and-core-safety`, `h2-design-rule-bypasses`, `h3-fp32-fields-and-gpu`, `h4-mesh-and-io-correctness`, `h5-memory-and-cancellation`, `h6-app-and-cli-fixes`), starting with **H1**. Structural monolith file-splitting (`MainWindow`, `rebuild.cpp`, `mesher.cpp`) is **deferred** until after the H1–H6 bug fixes land.

## Phase sequence

```
H1 (CI safety net & core UB/leaks)
 ├──> H2 (Seven design-rule bypasses) ──> H4 (Mesh geometry & file I/O) ─┐
 └──> H3 (FP32 fields & vs_gpu, v2) ─────────────────────────────────────┴──> H5 (Memory, cancel & perf) ──> H6 (vs_app & CLI bug fixes)
                                                                                                                  └──> Deferred: structural decomposition
```

| Phase | Branch | Scope | Target verification |
| --- | --- | --- | --- |
| **H1** | `h1-ci-and-core-safety` | `vs_app_core` split, ASan/UBSan/TSan CI, OCCT/PlaneGCS/eval/mesher UB & leak fixes | Linux CI + sanitizers + Windows MSVC/app |
| **H2** | `h2-design-rule-bypasses` | Close Containment, Encapsulation, One-Way Flow, and profile/rebuild bypasses | Linux CI + Windows MSVC |
| **H3** | `h3-fp32-fields-and-gpu` | Canonical FP32 fields & unmerged nets, `vs_gpu` concurrency/GL fixes, `kEngineVersion = 2`, ADR 0023 | Windows GPU `vs_gpu_tests` + `bench-gyroid` + Linux CI |
| **H4** | `h4-mesh-and-io-correctness` | Custom CDT predicates/sweep-line, L2 band seam & tessellation fixes, OBJ/3MF/`.vsproj` correctness fixes | Linux CI + Windows MSVC |
| **H5** | `h5-memory-and-cancellation` | Shared-blob `BlobStore`, bounded LRU caches, end-to-end cancellation, peak-RAM & CPU wins | Linux CI + Windows `bench-gyroid` + app tests |
| **H6** | `h6-app-and-cli-fixes` | `vs_app` dialog/lifetime/viewport/node-editor/sketch/export bug fixes & `vstudio-cli` parity | Windows `[gui]` + headless `vs_app_core_tests` + CLI |

---

## Phase H1 — CI visibility, sanitizers & core safety foundations

**Branch:** `h1-ci-and-core-safety` (start here)

**Goal:** Eliminate the highest-risk C++ lifetime/leak/immutability hazards and turn on automated sanitizer + headless `vs_app` testing before touching geometry or GPU precision.

### Build & CI infrastructure
- **Split `vs_app_core` from `vs_app_ui`** (`apps/vs_app/CMakeLists.txt`):
  - Move the 13 Qt-free, OCCT-viz-free files (`authoring`, `dimension_drag`, `exporter`, `graph_edit`, `guidance`, `history`, `node_preview`, `outline`, `picking`, `reference_choices`, `scene`, `ship`, `sketch_editor`) into a `vs_app_core` static library built unconditionally (even when `VS_BUILD_APP=OFF`) and tested on Linux & Windows CI via `vs_app_core_tests`.
  - Keep Qt/OCCT widgets (`main_window`, `viewport`, `session`, `node_editor`, `sketch_mode`, `feature_dialog`, `model_dialogs`, `widgets`) in `vs_app_ui` behind `VS_BUILD_APP=ON`.
- **Add Linux sanitizer CI jobs** (`.github/workflows/ci.yml`):
  - Add a Clang `ASan + UBSan` job and a `TSan` job running all library suites + `vs_app_core_tests`.
  - Move `permissions: contents: write` off the matrix `build` job into a dedicated `publish-cache` job running only on `main` / `workflow_dispatch`.
- **Build hygiene:**
  - Add `check_public_headers.cmake` for `vs_proc` (verifying Manifold stays private, matching `vs_kernel` and `vs_sketch`).
  - Tighten `PUBLIC_LINKS` in `libs/vs_proc/CMakeLists.txt` and `libs/vs_gpu/CMakeLists.txt` to actual public-header dependencies.
  - Extract a shared `vs_model_test_fixtures` target so `../vs_model/tests/{curved,nested}.cpp` are no longer recompiled into 5+ test binaries.

### Core safety & UB fixes
- **`vs_kernel` immutability, XCAF leak & exception guards:**
  - Call `op.SetNonDestructive(true)` in `run_boolean()` and `common_faces()` (`libs/vs_kernel/src/operations.cpp:85-89, 143-147`) so shared `Shape` handles are never mutated in place.
  - Wrap `new_xcaf_document()` in an RAII guard calling `XCAFApp_Application::GetApplication()->Close(doc)` on all exit paths in `write_step()` and `read_step_parts()` (`libs/vs_kernel/src/exchange.cpp:134-138, 166, 210`).
  - Fix null `poly->Parameters()` dereference in `libs/vs_kernel/src/tessellate.cpp:163, 251`; add `.is_empty()` checks and `detail::guarded` to `edge_point`, `edge_uv`, `face_point` (`tessellate.cpp:336-362`), `queries.cpp:127-165` (`SolidClassifier`, `classify`, `distance_to`, `bounding_box`), and `check.cpp:95-117`; reject `NaN` in `edge_parameter()` (`tessellate.cpp:294-334`), `classify()`, and `distance_to()`.
- **`vs_sketch` PlaneGCS bugs:**
  - Call the 5-argument `sys_.addConstraintP2PAngle(l.p1, l.p2, value, 0.0, tag)` in `libs/vs_sketch/src/solver.cpp:265` (avoiding the ignored `/*tagId*/` in `third_party/planegcs/src/GCS.cpp:649-652`).
  - Run `all_hold()` unconditionally *after* `applySolution()` (`solver.cpp:402-418`), failing on `std::isnan(err)`; validate `*c.rad > 0` and finiteness before `write_back()` (`solver.cpp:338-341`).
- **`vs_eval` determinism check:**
  - Set `c.status = VolumeStatus::Failed` when `eval.nondeterministic` triggers (`libs/vs_eval/src/engine.cpp:490-496`) so nondeterministic outputs never become `Current`, overwrite `last_valid_`, unblock dependants, or enter the disk cache.
  - Guard the second determinism pass against mid-run cancellation (`engine.cpp:472`).
- **`vs_proc` immediate UB & containment push:**
  - Hoist `MergeWorker` / `workers` declarations above `merger.finish()` in `merge_net()` (`libs/vs_proc/src/mesher.cpp:567-630`) so `Fans` spans never point into freed arena blocks.
  - Fix the inverted sign fallback in `Builder::gradient()` (`mesher.cpp:1309-1319`: `signed_distance(p) < 0 ? -away : away`) and scale the push slack in `Builder::contain()` (`mesher.cpp:1183-1255`) above half an FP32 ULP at `positions_[v]`.

### Acceptance criteria (H1)
- Linux GCC, Clang, ASan+UBSan, TSan, and Windows MSVC (including `vs_app_core_tests`) pass.
- New regression tests: XCAF document close (repeated `write_step`/`read_step_parts` without document leaks), 1-line `Angle` conflict reporting & post-solve redundant violation, deliberate nondeterministic `ContentsGenerator` producing `VolumeStatus::Failed` and skipping disk cache, `Builder::gradient()` inward fallback on symmetric thin walls.

---

## Phase H2 — Closing all seven design-rule bypasses

**Branch:** `h2-design-rule-bypasses`

**Goal:** Eliminate every verified bypass of Containment, Encapsulation, One-Way Data Flow, and structural validity across flat, curved (L2), and composite (L3) models.

### Rule 5 — Containment & layout validation (`vs_validate`)
- **Curved B-rep containment (`libs/vs_validate/src/containment.cpp:61-87`):** When `mass->volume > 0`, measure reach across tessellated surface/edge points of `*outside` (not solely topological vertices) so bulging spheres, cylinders, domes, and fillets cannot slip through with `reach == 0`. Report failing `mass_properties()` as `validate.undecided`.
- **Mesh containment safety & threading (`containment.cpp:112-274`):**
  - Validate `mesh.indices` before returning early on `mesh.positions.empty()`; reject non-finite `positions` or `centroid_slack` with `validate.undecided`.
  - Copy `boundary` (`BRepBuilderAPI_Copy`) per worker thread so parallel chunks never share a mutable `TopoDS_Shape`; construct `SolidClassifier` once per worker thread instead of per 2,048-sample chunk; make `SolidClassifier` non-copyable (or deep-copying).
  - Fix the octree stop-condition typo at `containment.cpp:144` (`child.size() == group_size`, saved before `group.clear()`), and break early out of the face-distance loop as soon as `d < r`.
- **L3 nesting & connection checks (`libs/vs_validate/src/layout.cpp:153-180, 421-445`):**
  - Replace the dimensionally wrong `tol_ * parent->box.volume()` heuristic with an exact box-fill check (6 planar axis-aligned faces matching `parent->box`) and cache per-parent classification rather than recomputing per child.
  - Strengthen `validate.connection.off_boundary` so patches poking through or floating inside a solid (`InnerSolution()`) fail, and enforce clearance gap bounds on `Clearance` surfaces without exempting zero-distance contact on unrelated faces.
  - Route `validate.undecided` dynamically by its first subject (Contents / ConnectionSurface / Layout) and handle `PublishedOutput` and `ConnectionSurface` in `level_of_subject()` (`libs/vs_validate/src/registry.cpp:56-77`).

### Rule 1 & Rule 6 — Encapsulation, identity & dependency graph (`vs_model`, `vs_feature`)
- **Reject null IDs & unify ID walking:**
  - Reject `Id::null()` (`0000…0000`) in `read_id()` across `vs_model`, `vs_feature`, and `vs_sketch` (for authored object IDs) and in `Checker::unique_ids()` (`libs/vs_model/src/check.cpp:69-88`).
  - Consolidate the four drifted tree-ID walkers (`tree_holds`, `layout_holds`, `note_tree`, `in_tree`) into one shared helper that includes sketch constraints and `sketch.id()`.
  - Catch self-parenting composites (`v.parent == v.id`) explicitly in `volume_structure()` (`check.cpp:173-188`).
- **Close `DependencyGraph` holes (`libs/vs_model/src/dependency.cpp:151-221`):**
  - Chain `changes_body` features in `tree_edges()` so `ThroughAll` extrudes, cuts, shells, fillets, chamfers, and patterns depend transitively on all earlier body features.
  - Resolve `NodeKind::VolumeBoundary` and `NodeKind::ConnectionSurface` inside `uses_layout()` (matching `tree_edges()`), closing L3 cycle bypasses and stale-wave scheduling.
- **Rebuild & model build integrity:**
  - In `vs_feature::rebuild()` (`libs/vs_feature/src/rebuild.cpp:1545-1572`): verify upstream feature definitions, sketches, and `ProductSettings` are unchanged before reusing `prev`; stop `ExternalInputs` from shadowing failed tree variables (`L1545-1550`).
  - In `libs/vs_model/src/build.cpp`: preserve failed composite `contents_rebuild` across `apply_contents()` (`L706-709, 849-855`); fail target boundaries when a keep-out volume cutter failed (`L449-454`); exclude failed parametric solids from composite `measures` (`L935`).
- **2D profile extraction (`libs/vs_sketch/src/profile.cpp`):**
  - Add per-loop 2D AABBs (fixing `O(L²P²)` scaling), check segment-segment intersections (catching crossing `+` rectangles, self-intersecting bowties, and coincident circles), and use deflection-driven sampling for large arcs.

### Acceptance criteria (H2)
- Dedicated regression tests for every bypass: bulging sphere/cylinder containment, `NaN` mesh vertex, 10 mm notch on 1,000 mm composite, null-ID volume rejection, self-parenting composite, `ThroughAll` feedback cycle, L3 delegated surface wave ordering, crossing `+` sketch profile, and edited-upstream incremental rebuild.

---

## Phase H3 — Canonical FP32 fields, unmerged surface nets & `vs_gpu` overhaul (`kEngineVersion = 2`)

**Branch:** `h3-fp32-fields-and-gpu`

**Goal:** Move procedural field sampling and unmerged surface nets to canonical IEEE-754 FP32 so consumer GPUs decisively beat the CPU, fix all `vs_gpu` concurrency/correctness defects, eliminate cross-platform BVH/trig non-determinism, and bump `kEngineVersion` once cleanly.

### Versioning, cache keys & graph checks
- Bump `core::kEngineVersion` (`libs/vs_core/include/vs/core/version.hpp:16`) from `1` to `2`.
- Hash `ProductSettings` into `eval::cache_key()` (`libs/vs_eval/src/cache.cpp:50-108`) and `inputs.settings` into `proc::evaluate_graph()`'s `NodeCache` key (`libs/vs_proc/src/evaluate.cpp:190-206`).
- Split `"proc.graph.output"` into `"proc.graph.no_output"` (unfinished) vs `"proc.graph.multiple_outputs"` (hard error rejected by `edit()`, `libs/vs_proc/src/check.cpp:146-148`).
- Fix curved connection surface normal handling in `scatter.surface` and `input.surface` (`nodes_frames.cpp:261`, `nodes.cpp:200`).

### Deterministic, local-recentered `TriangleDistance` (`libs/vs_proc/src/distance.cpp`)
- Sort `order_[first .. first + count)` inside every BVH leaf (`distance.cpp:152`) so leaf visit order is identical across MSVC, libstdc++, and libc++.
- Replace `std::atan2` in `corner_angle()` (`distance.cpp:72-76`) with deterministic arithmetic-only math, and guard `0.0 / 0.0` in `closest_on_triangle()` (`distance.cpp:47-68`).
- Store a local-origin-recentered FP32 BVH representation (`dvec3 origin_ = bounds_.center()`, FP32 local vertices/normals/boxes) with `distance_f(fvec3)` / `signed_distance_f(fvec3)` using a numerically conditioned triangle distance routine for field sampling & GPU upload.
- Keep FP64 `nearest(dvec3)` / `signed_distance(dvec3)` for CPU `Builder::contain()`.

### Canonical FP32 `FieldExpr` & `unmerged_net` (`vs_proc`)
- **Fields (`libs/vs_proc/include/vs/proc/field.hpp`, `src/field.cpp`):**
  - Evaluate `FieldExpr` and `sample_cpu()` in canonical IEEE-754 FP32 (with `-ffp-contract=off` / `/fp:precise`).
  - In `tpms_distance()`, reduce `(p - origin) / cell` to `[0, 1)` via `u - floor(u)` **before** multiplying by `2π`, then evaluate an FP32 `sin_det_f` / `cos_det_f` polynomial on `[0, 2π)` (eliminating `sin_det(1e20)` overflow while keeping `< 1 ULP` FP32 accuracy). Retain FP64 `sin_det` / `cos_det` for `nodes_frames.cpp` and `nodes_mesh.cpp`.
- **Unmerged surface nets (`mesher.hpp`, `mesher.cpp`, `surface_nets_grid.hpp`, `surface_nets_reference.cpp`):**
  - Compute `UnmergedNet::sums` in **grid-local `fvec3`** (`0..nx/ny/nz`) and add `grid.origin` in FP64 during `merge_net()` finalization (zero world-origin precision loss, half the memory footprint).
  - Compare `length_squared(cross(b - a, c - a))` in FP32 inside `edge_polygon()` so neither CPU nor GPU needs `sqrt` for fan-apex selection.
  - Fix `merge_net()` thin-grid memory explosion (`mesher.cpp:576`) by assigning stride weights `{1, 3, 9}` sorted by axis length descending (or sparse wave buckets).
- **Shared layout contract:** Share field-buffer layout offsets, `LatticeCell`/`TpmsSurface` enum ordinals, and `kMaxBvhDepth` between `vs_proc` and `vs_gpu` with `static_assert`s.

### `vs_gpu` FP32 port, concurrency & GL safety
- **FP32 shaders (`libs/vs_gpu/src/glsl.cpp`, `src/meshing.cpp`):**
  - Emit `precise float` / `vec3` / `uintBitsToFloat` instead of `double` / `dvec3` / `packDouble2x32`; implement a subnormal-safe FP32 `sqrt_rn(float)` (fixing the underflow bug at `glsl.cpp:118-134`); drop `sqrt_rn` from `meshing.cpp`; emit `MESH{}` definitions directly from `meshes_.size()` (`glsl.cpp:575-582`).
- **Concurrency & GL robustness (`device.cpp`, `sampler.cpp`, `gl_context.cpp`):**
  - Fix `Device::run()` stack `done_cv` UAF (`device.cpp:64-83`: notify under `done_mutex` or use `std::promise`/`future`), catch exceptions in `Device::loop()`, and guard `stopping_` in `run()`.
  - Add explicit `gpu::shutdown()` (called before exit in `vs_app` / `vstudio-cli`) instead of destroying `Device` + calling `FreeLibrary(opengl32.dll)` during `atexit`.
  - Fix `Buffer::Buffer()` OOB read for `0 < bytes < 16` (`device.cpp:127-133`) and use `GL_STATIC_DRAW` for read-only SSBOs.
  - Check `glGetError()` and `GL_MAX_SHADER_STORAGE_BLOCK_SIZE` in `sample_gpu()` and `meshing.cpp` (never returning silent zeros); use 2D compute dispatches (`gx, gy, 1`) so wide flat grids never exceed 65,535 workgroups.
  - Fix `calibrate()` (`sampler.cpp:475-485`) to check `std::optional::has_value()` so a failing GPU stage never wins calibration; lock `s.status.why` and guard concurrent `install()`; use `HWND_MESSAGE` for the hidden WGL window (`gl_context.cpp:47`) and query `wglCreateContextAttribsARB` when available.
- **Tests & ADR:** Update `test_field.cpp`, `test_acceptance.cpp`, `test_infill.cpp`, `test_distance.cpp`, `test_parallel_mesher.cpp`, and `vs_gpu/tests/test_arithmetic.cpp` (covering subnormals and full FP32 exponent range). Record Windows RTX 2070 Super `bench-gyroid` measurements in `docs/decisions/0023-fp32-procedural-fields.md`.

### Acceptance criteria (H3)
- On the Windows desktop GPU: `vs_gpu_tests` passes bit-for-bit CPU==GPU across arithmetic, fields, templates, and meshing; `bench-gyroid --size 100 --cell 0.5` shows calibration enabling GPU stages and clearly beating the 6-core CPU.
- On Linux & Windows: all `vs_proc` and `vs_eval` tests pass identically across 1, 4, and 16 threads, including containment checks on parts placed near `±1,000 mm`.

---

## Phase H4 — Mesh geometry & file I/O correctness

**Branch:** `h4-mesh-and-io-correctness`

**Goal:** Fix algorithmic failure modes in custom CDT and L2 curved seams, plus real-world crash, UB, and parser-correctness bugs in OBJ, 3MF, STL, and `.vsproj` I/O.

### Custom CDT (`libs/vs_mesh/src/triangulate.cpp`)
- Symmetrize `orient_eps(a, b, c)` across all three edge lengths (or adopt Shewchuk adaptive exact `orient2d`/`incircle`), eliminating `Cdt::recover()` ↔ `convex_flip_ok()` deadlocks and sliver inversions.
- Add a deterministic flip-count cap in `Cdt::legalise()` (`L244-262`); guard `constrained(x, y)` before flipping in `Cdt::recover()`; replace `orient * orient < 0` with sign comparisons (`L368-369`).
- Replace `check_segments()`'s mean-cell hash grid (`L487-526`) with an X-sorted sweep-line + AABB pruning so mixed-scale CAD outlines never OOM or degrade to `O(M²)`.

### Curved band seams, conforming tessellation & welding (`vs_mesh`)
- **Band seams (`seam.cpp:311-320, 444-490`):** Cap effective band spacing (or insert interior column constraints) so no band triangle ever spans `≥ πr` or collapses two diagonals onto a 4-triangle cut edge.
- **Combinatorial weld facing (`weld.cpp:152-165`):** Judge seam triangle orientation by cyclic permutation parity against `seam.triangles` / `seam.hole_fills` instead of geometric `triangle_uv` cross products; include `hole_fills` in conformity checks and fix `"0 of N missing"` on duplicate seam triangles.
- **Conforming tessellation (`tessellate.cpp`):**
  - Handle periodic `±2πr` wraparound in `divide_on_seam()` UV-box culling (`L303-335`) and select the minimum-span arc pair on closed circular edges (`L359-378`).
  - Test an interior surface sample in `patch_piece()` (`L445-471`) so small complement faces on nearly-full cylinders aren't misclassified as inside the patch.
  - Fix orphaned Steiner points from collapsed wires (`L547-559`), deduplicate BRepMesh interior points against curved remainder lattices (`L670-694`), and propagate `make_cylinder_face` errors (`L823-827`).
- **Zero canonicalization & mesh invariants:** Canonicalize `-0.0f → +0.0f` in `attach_seam_vertices()` (`seam.cpp:639-642`) and `Builder::vertex()` (`io.cpp:44-51`); make `Mesh::append(m)` self-append-safe; check finiteness in `Mesh::check()`; shift by `p0` in `enclosed_volume()` (`mesh.cpp:79-89`) and `eval::measure_mesh()` (`libs/vs_eval/src/measure.cpp:25-45`).

### Real-world file I/O correctness (`vs_mesh/src/io.cpp`, `vs_model`, `vs_eval`)
- **`read_obj()` (`io.cpp:532-539`):** Replace `-k > n` with `k < -n` (fixing `LLONG_MIN` signed-overflow OOB read) and strip inline `#` comments.
- **`read_3mf()` (`io.cpp:608-626`):** Add `!tag.closing` to `vertex`, `triangle`, `component`, and `item`; strip XML namespace prefixes (`m:model` → `model`); decode numeric character entities (`&#...;`); cache `Object*` instead of looking up `objects[current]` per vertex/triangle.
- **Finiteness on import/export:** Verify `std::isfinite` on narrowed `float` coordinates in ASCII STL/OBJ/3MF readers (stopping `Inf` from bypassing `import_mesh_volume()`) and validate `Mesh::check()` in `write_stl` / `write_3mf`.
- **`.vsproj` & cache file hygiene:**
  - Verify `fs::is_regular_file(p, ec)` in `libs/vs_model/src/project.cpp:44-57` (preventing `blobs/.` directory-open `bad_alloc`) and pass `ec` to `is_regular_file` at lines `187, 199, 212`.
  - Check `f.close()` / stream state and use per-write temp names in `project.cpp:61-83` and `libs/vs_eval/src/cache.cpp:273-285`; validate enum ranges in `cache.cpp:354-359`.
  - Validate 64-bit JSON `"version"` integer range and wrong member types (`optional_member`) in `libs/vs_model/src/serialize.cpp:604-614`.

### Acceptance criteria (H4)
- Regression tests for coarse band seam welding, band-cut edge tessellation, 350° curved complement face, `-0.0f` STL/baked welding, `read_obj` `LLONG_MIN` and inline `#` comments, non-self-closing & namespace-prefixed 3MF files, `blobs/.` rejection, and mixed-scale CDT outlines.

---

## Phase H5 — Memory bounds, end-to-end cancellation & performance

**Branch:** `h5-memory-and-cancellation`

**Goal:** Eliminate multi-GB memory growth during interactive sessions, make heavy procedural generation genuinely cancellable, and remove the largest mesh/evaluation hotspots.

### Shared blobs & bounded caches
- **Shared blobs in `model::BlobStore` (`libs/vs_model/include/vs/model/project.hpp:88-90`):** Store blobs as `std::shared_ptr<const Blob>` (matching `history.hpp:17-18`'s documented invariant), removing full-blob deep copies on every undo step, dialog live-preview tick, drag frame, `import_mesh_volume()`, and `import_volume()`.
- **Bounded LRU caches:**
  - Add configurable byte-budget LRU eviction to `eval::ContentsCache::memory_` (`libs/vs_eval/src/cache.cpp:408-465`, accounting for `Mesh` + `FieldContents`) and `proc::NodeCache` (`libs/vs_proc/src/evaluate.cpp:26-30`, replacing the 4,096-entry cliff clear).
  - Cap `History::undo_` depth and `vs_gpu::Device::programs_`; clear `generator_->cache()` and `preview_->inputs` in `Session::set_project()` (`apps/vs_app/src/session.cpp:251-254`).

### End-to-end cancellation
- Thread `const eval::CancellationToken*` through `mesh_contents()`, `clip_to_boundary()`, `surface_nets()`, `unmerged_net()`, `merge_net()` (checked per wave), `Builder::stitch()`, `Builder::contain()`, and `NodeContext` (`convert.field_to_mesh`, `scatter.*`).
- Wire `Session::cancel_preview()` (`session.cpp:115-119`) to an actual `CancellationToken` passed into `inspect_node()`/`preview()`, and bail before `finish_snapshot()` in `Session::evaluate_job()` when superseded or cancelled.

### Peak-RAM & CPU hotspot wins
- Scope `Builder::stitch()` (`libs/vs_proc/src/mesher.cpp:853-915`) edge and fan maps strictly to candidate/rim triangles (saving ~1.5–2 GB on 10M-triangle gyroids).
- Change `weld()` (`libs/vs_mesh/src/weld.cpp:70-119`) to take `std::span<const Mesh* const>` (removing `combine.cpp:108`'s copy) and index only defect edges/vertices in `weld.cpp:250-264`.
- Use `pln.Location()` instead of `kernel::face_plane()` (`BRepGProp::SurfaceProperties`) in `faces_of()` (`libs/vs_feature/src/rebuild.cpp:56-59`); switch multi-profile `Extrude`/`Revolve` fuses to multi-tool `Builder::merge()` (`L942-948, 1000-1006`); remove the double `build_graph()` in `VariableTable::evaluate()` (`libs/vs_feature/src/variables.cpp:305-306`).
- Batch canonicalized floats into BLAKE3 blocks in `SampledField::hash()` (`field.cpp:479`) and use `format_to` + `reserve` in `io.cpp`'s 3MF/STL writers and readers.

### Acceptance criteria (H5)
- Tests verify `History` shares `Blob` pointers across 50 edits, `ContentsCache` and `NodeCache` evict under byte caps, and cancelling mid-`mesh_contents()` returns `Stale` cleanly in < 250 ms without caching partial output.
- Measured peak RSS reduction on `bench-gyroid --size 100 --cell 0.5`.

---

## Phase H6 — `vs_app` & `vstudio-cli` bug fixes

**Branch:** `h6-app-and-cli-fixes`

**Goal:** Fix all verified desktop UI/UX, Qt lifetime, viewport, and CLI bugs. (Structural monolith file-splitting is explicitly deferred to a later follow-up once bug fixes land.)

### `vs_app` fixes
- **Dialogs & authoring:**
  - Fix `FeatureDialog::complete()` (`apps/vs_app/src/feature_dialog.cpp:624-649`) to validate only the active `QStackedWidget` page (unblocking editing of `PlaneThroughPoints`, `PlaneAxisAngle`, `PointAtRef`, `PointAxisPlane`); preserve Fillet/Chamfer radius text on edge add/pick (`L305-329`); validate numeric/param fields in `ConnectionDialog`/`VolumeDialog`/`KeepOutDialog` (`model_dialogs.cpp`); call `end_pick()` on page switch.
  - Fix `MainWindow` sketch-insert double-close (`main_window.cpp:1278-1282, 1627`), `QPointer<EditDialog>` + null `screen()` check in `main.cpp:110-174`, and add an unsaved-changes confirmation prompt on Close/New/Open.
- **Session & viewport:**
  - Synchronize `Session::preparer_` (`session.cpp:81, 221, 375`), coalesce `drag_preview()` to a single pending slot, gate `progressed()` by generation, and clear progress UI when `drag_preview()` supersedes `regenerate()`.
  - Fix `Viewport` picking on B-reps > 60k triangles (`viewport.cpp:121-131`: populate `face_ranges`/`edge_lines` per chunk), clear `last_pick_valid` in `show_scene()`, reject behind-camera handle hits in `project()`, and stop forcing full `view->Invalidate()` on hover repaints.
- **Node editor, sketch mode & export:**
  - Preserve wire direction (`w.from_output`) into `connect_ports()` (`node_editor.cpp:463-467`, `graph_edit.cpp:199-214`); parent `QMenu` in `GraphScene::contextMenuEvent()` (`L495`); disconnect old `NodeForm` widgets before `deleteLater()` (`L1265-1279`).
  - Suspend `MainWindow`'s `Ctrl+Z`/`Ctrl+Y`/`Esc`/`F` shortcuts while `SketchMode` is active; stop `refresh()` from overwriting `set_dimension_text()` errors (`sketch_mode.cpp:586-587`); fix signed drag clamping and degenerate-click undo steps in `sketch_editor.cpp`.
  - Reuse the mesh weld in `ship.cpp` instead of welding twice on STL/3MF export, and stop mesh-weld errors from blocking STEP export (`ship.cpp:42-48`); compare `History::new_errors()` by `(code, subjects)` rather than formatted message text.
  - Fix L3 composite color indexing (`scene.cpp:233` vs `main_window.cpp:844`) and check `has_children` in `make_placeholder()`.

### `vstudio-cli` fixes
- Add `--3mf` and `--per-volume` to `rebuild` (or run `export` through `eval::Engine` + `proc::install()`) so procedural projects can export 3MF from the CLI; support zip `.vsproj` caches in `rebuild --cache`; defer `--gpu` initialization until a command needs it; fix `bench-gyroid --threads` integer parsing + write-error checks; deduplicate `to_path()`; include `vs_gpu` in `selftest`.

### Acceptance criteria (H6)
- Headless `vs_app_core_tests` cover `DatumPlane`/`DatumPoint` authoring, bidirectional same-name port wiring, STEP export with unweldable mesh, and composite color/placeholder rules.
- CLI tests cover procedural 3MF export and zip `--cache` round-trip.
- Windows `[gui]` tests pass with zero regressions.

---

## Deferred follow-ups (post-H6)

- **Structural monolith decomposition:** Extract `ProjectIoController` and `DialogCoordinator` from `apps/vs_app/src/main_window.cpp` (2,061 LOC); split `libs/vs_feature/src/rebuild.cpp` (1,583 LOC), `libs/vs_proc/src/mesher.cpp` (1,456 LOC), `apps/vs_app/src/node_editor.cpp` (1,472 LOC), and `libs/vs_mesh/src/tessellate.cpp` (841 LOC) into focused internal translation units.
- **Topological naming evolution:** Expose OCCT `Generated`/`Modified` face-history tags from `vs_kernel` so `vs_feature`'s `FaceRef` no longer relies solely on unbounded infinite-plane/cylinder matching.
- **Cross-platform UI & GPU backends:** Port `vs_app_ui`'s `Viewport` (`Xw_Window` / `Cocoa_Window`) and add EGL/Vulkan/Metal backends to `vs_gpu` (now unblocked by H3's FP32 fields).

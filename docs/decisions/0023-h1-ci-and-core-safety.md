# 0023: H1, CI and core safety

Nov 21, 2026 · plan `docs/plans/H-hardening.md` (PR 28), phase H1

## Context

H1 starts the hardening pass: sanitizers and headless app-logic tests in
CI, then the highest-risk lifetime, leak and immutability items from the
audit. The plan's rule is "verify, then fix": each item starts with a test
that fails on `main`, and an item that can't be reproduced is dropped.

## Decisions

**Build and CI.**

- `apps/vs_app` is split into `vs_app_core` (the 13 Qt-free files) and
  `vs_app_ui` (Qt, the OCCT viewport, the session). `vs_app_core` and
  `vs_app_core_tests` build in every configuration, so every CI job runs
  them; `vs_app_ui`, `vs_app` and `vs_app_tests` still need
  `VS_BUILD_APP=ON`. The Qt-free test cases moved with their files
  (`test_ship.cpp`'s validate case stayed; its GUI case is
  `test_ship_ui.cpp`), and `samples::wait_until()` moved to `tests/wait.*`
  so `samples.*` is Qt-free. **The `windows-msvc-app` build is unchecked
  until H6**, which verifies it first.
- `VS_SANITIZE` (a list such as `address;undefined`) instruments everything
  this project builds, not vcpkg's ports. The `linux-clang-asan` preset sets
  it with Clang, `RelWithDebInfo`; its test preset stops on the first
  report (`halt_on_error`, `abort_on_error`) and runs LeakSanitizer with
  `scripts/sanitizers/lsan.supp`. CI runs it as a fourth matrix job. No
  TSan (plan decision 6).
- The CI `build` job is read-only. Publishing the vcpkg cache moved to a
  `publish-cache` job (`contents: write`, `push` to `main` and manual runs
  only), which restores `linux-gcc-release`'s actions cache and gets the
  installed ports' ABI files as a small artifact. Linux jobs fall back to
  any Linux preset's cache (`restore-keys`), since every Linux preset uses
  the same x64-linux binaries, so the new job doesn't build OCCT.
- `vs_model_test_fixtures` compiles the shared test models (bracket,
  curved, nested, rack) once; suites link it.
- `vs_proc` gets a public-header check for Manifold, and `vs_proc` and
  `vs_gpu` list only what their public headers need in `PUBLIC_LINKS`.

**Reproduced and fixed** (each has an `[h1]` test that failed on `main`):

| Item | Failing test | Fix |
| --- | --- | --- |
| Booleans changed their inputs | `Booleans leave their inputs unchanged`: a fuzzy union widened the tool's edge tolerance from 1e-7 to 5e-7 mm and changed its `shape_hash` | `SetNonDestructive(true)` in `run_boolean()` and `common_faces()` |
| XCAF documents never closed | `STEP assemblies leave no XCAF document open`: 8 documents left open after 3 round trips and 3 failures | `XcafDocument`, closing on every exit of `write_step()` / `read_step_parts()` |
| Empty handles crashed point queries | `Edge and face points of empty handles are NaN`: SIGSEGV | `edge_point()`, `edge_uv()`, `face_point()` answer NaN for empty handles and catch kernel failures |
| NaN points in `edge_parameter()` | `Edge parameters of empty edges and NaN points are none`: a NaN point or tolerance gave a parameter | nullopt for points or tolerances that aren't finite |
| One-line angles lost their tag | `A one-line angle that conflicts is named` (not named) and `... repeats another constraint is named` (not solved) | the 5-argument `addConstraintP2PAngle()` |
| Nondeterministic contents stayed current | `Nondeterministic contents fail and are never cached`: Current, and two files in the disk cache | `eval.nondeterministic` sets `Failed`; the second determinism run is skipped when cancelled |

**Not reproduced, dropped:**

- `merge_net()` freeing workers' arenas before `merger.finish()`: `finish()`
  reads only the runs' sizes (`fan_[v].empty()`), never their elements, so
  nothing reads freed memory.
- `Builder::gradient()`'s fallback sign: it is only reached where the
  central differences cancel exactly (the middle of a symmetric wall), and
  there both directions are equally good; a test couldn't tell them apart.
- `Builder::contain()`'s push slack at large coordinates: an L-shaped
  boundary at (900, −700, 650) mm meshed contained, every rounded vertex
  inside by the inset.
- A redundant sketch constraint violated after a successful solve: a
  rectangle with a diagonal 0.01 mm off either solved with it holding or
  reported the conflict.
- NaN points in `classify()` (already Outside) and `distance_to()` (already
  an error). The null `Parameters()` in `tessellate()` and empty handles in
  `check_validity()` / `audit_tolerances()` / the queries weren't reachable
  from the public API (empty handles already answer "nothing"; tested).

**Moved.** `Device::run()`'s use-after-free of its stack condition variable
(confirmed by reading the code) stays in H6, as the plan has it: on Linux
there is no GL context, so `run()` returns before queueing anything and no
test here can reach it.

**Branch.** Cloud sessions push to their own branch, so H1 is on
`claude/project-thread-zz738a` rather than `h1-ci-and-core-safety`.

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
# Linux (git, cmake 3.25+, ninja, GCC 12+ or Clang 16+, curl, zip, unzip, tar, pkg-config)
scripts/build.sh                    # linux-gcc-release
scripts/build.sh linux-clang-debug
```

Both scripts accept `--clean`/`-Clean` and `--skip-tests`/`-SkipTests`. They
use `VCPKG_ROOT` if it points at a git clone of vcpkg; otherwise they clone
vcpkg into a per-user cache (`%LOCALAPPDATA%\vstudio\vcpkg` or
`~/.cache/vstudio/vcpkg`) at the commit pinned by `builtin-baseline` in
`vcpkg.json`. The first build installs dependencies and takes several
minutes; later builds reuse vcpkg's binary cache.

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
| `VS_BUILD_APP` | OFF | Builds the Qt app (`vs_app`) and turns on the vcpkg `app` feature (Qt 6). Only the `windows-msvc-app` preset sets it. |
| `VS_BUILD_TESTS` | ON | Builds the Catch2 suites and registers them with CTest. |
| `VS_WARNINGS_AS_ERRORS` | ON | `/WX` or `-Werror` on first-party code. |

Heavy dependencies sit behind vcpkg manifest features so that each milestone
installs only what it uses (see `docs/decisions/0000-optional-vcpkg-features.md`):
`kernel` (OCCT, from M1), `sketch` (Eigen, from M2), `proc` (Manifold, from
M12) and `app` (Qt). A milestone that starts using one appends it to
`VCPKG_MANIFEST_FEATURES` in the top-level `CMakeLists.txt`, before `project()`.

## Layout

```
cmake/                  build helpers: compile options, module rule, vs_add_library
libs/<module>/          one directory per library
  include/vs/<short>/   public headers (namespace vs::<short>)
  src/                  sources and private headers
  tests/                Catch2 tests, built as <module>_tests
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

## Code style

- C++20, warnings as errors. `#pragma once`. Namespaces `vs::<short>`.
- Types `PascalCase`, functions and variables `snake_case`, constants `kPascalCase`.
- Public headers document intent and invariants; keep implementation detail in `src/`.

# 0000: Heavy dependencies are optional vcpkg features

Nov 18, 2026 · M0

## Context

The plan's M0 scope lists a vcpkg manifest with OCCT, Eigen, Manifold,
Catch2, BLAKE3, fmt, spdlog and nlohmann-json, with Qt optional behind
`VS_BUILD_APP`. Building OCCT (and Qt) from source through vcpkg takes a long
time on a clean machine or CI runner, and M0 doesn't use OCCT, Eigen or
Manifold yet.

## Decision

All of them are in `vcpkg.json`, but only the light dependencies used from M0
(Catch2, BLAKE3, fmt, spdlog, nlohmann-json) are unconditional. The others are
manifest features:

| Feature | Dependency | Turned on by |
| --- | --- | --- |
| `kernel` | `opencascade` | M1, when `vs_kernel` starts using OCCT |
| `sketch` | `eigen3` | M2, for the vendored PlaneGCS |
| `proc` | `manifold` | M12, for mesh booleans |
| `app` | `qtbase` (gui, widgets, opengl) | `VS_BUILD_APP=ON` (already wired) |

A milestone that starts using a dependency appends its feature to
`VCPKG_MANIFEST_FEATURES` in the top-level `CMakeLists.txt` (before
`project()`), unconditionally for libraries every build needs.

vcpkg itself is pinned by `builtin-baseline` in `vcpkg.json` (vcpkg tag
2026.10.16). The build scripts clone vcpkg at that commit unless `VCPKG_ROOT`
points at an existing clone.

## Consequences

- A clean clone builds and tests M0 in minutes rather than the better part of
  an hour, on both platforms and in CI.
- M1 pays the OCCT build cost once; vcpkg's binary cache (and the CI cache)
  keeps it from recurring.
- The `opencascade` port is requested with `default-features: false`; M1
  should add any port features it needs (for example `freetype` or `rapidjson`)
  rather than taking the defaults.
- The `app` feature's Qt feature list has not been built yet; M13 confirms it.

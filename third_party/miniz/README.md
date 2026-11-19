# miniz (vendored)

Deflate and zip archives for `.vsproj` zip files, used by `vs_model`'s
`src/zip.cpp`, and for 3MF packages, used by `vs_mesh`'s `src/io.cpp`
(M8). Both link it privately; no public header outside this directory
includes it.

- **Source:** miniz 3.0.2, the amalgamated `miniz.c` and `miniz.h` from the
  release asset `miniz-3.0.2.zip`
  (https://github.com/richgel999/miniz/releases/tag/3.0.2, SHA-256 of the
  zip: `ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5`).
- **Licence:** MIT (`LICENSE`).
- **Changes from upstream:** none. The target defines `MINIZ_NO_STDIO` and
  `MINIZ_NO_TIME` and `MINIZ_NO_ZLIB_COMPATIBLE_NAMES` (see `CMakeLists.txt`).

It is vendored rather than a vcpkg port because cloud sessions can't
download vcpkg port sources (decision 0006). To update it, replace the two
files in `src/` with those of a newer release and rerun `vs_model_tests` and `vs_mesh_tests`.

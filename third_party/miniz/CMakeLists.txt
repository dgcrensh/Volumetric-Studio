# miniz: zlib-compatible deflate and zip archives, vendored from the
# miniz 3.0.2 release (the amalgamated miniz.c and miniz.h). See README.md.
#
# Only vs_model links this target, privately, for .vsproj zip files; no
# public header includes it.

add_library(miniz STATIC src/miniz.c)
add_library(vs::miniz ALIAS miniz)

target_include_directories(miniz SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(miniz PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# No stdio (archives live in memory) and no clock: entries get a zero DOS
# timestamp, so the same project always makes the same bytes. No zlib-style
# macro names (crc32, compress, ...), which would rename our own functions.
target_compile_definitions(miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# Third-party code: no warnings, and never warnings as errors.
if(MSVC)
  target_compile_options(miniz PRIVATE /W0 /utf-8)
  target_compile_definitions(miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(miniz PRIVATE -w)
endif()
set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)

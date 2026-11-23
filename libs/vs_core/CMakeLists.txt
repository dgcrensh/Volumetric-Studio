find_package(blake3 CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# The git revision is captured at configure time; reconfigure to refresh it.
set(VS_GIT_REVISION "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --abbrev=12
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE git_out
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE git_result
    ERROR_QUIET)
  if(git_result EQUAL 0 AND git_out)
    set(VS_GIT_REVISION "${git_out}")
  endif()
endif()
configure_file(src/version.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/version.cpp" @ONLY)

vs_add_library(vs_core
  SOURCES
    src/diagnostic.cpp
    src/hash.cpp
    src/id.cpp
    src/log.cpp
    src/settings.cpp
    src/units.cpp
    src/xml.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/version.cpp"
  PUBLIC_LINKS
    fmt::fmt
    spdlog::spdlog
  PRIVATE_LINKS
    BLAKE3::blake3)

vs_add_library_tests(vs_core
  SOURCES
    tests/test_box.cpp
    tests/test_diagnostic.cpp
    tests/test_hash.cpp
    tests/test_hash_golden.cpp
    tests/test_id.cpp
    tests/test_mat.cpp
    tests/test_settings.cpp
    tests/test_transform.cpp
    tests/test_units.cpp
    tests/test_vec.cpp)

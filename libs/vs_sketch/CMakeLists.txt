# vs_sketch: sketch entities, constraints, the PlaneGCS wrapper and profile
# extraction.
#
# PlaneGCS (third_party/planegcs) and its Eigen dependency are linked
# privately: no public header includes them, so modules above this one never
# see solver types. The tests below enforce that.

find_package(nlohmann_json CONFIG REQUIRED)

vs_add_library(vs_sketch
  SOURCES
    src/module.cpp
    src/profile.cpp
    src/serialize.cpp
    src/sketch.cpp
    src/solver.cpp
  PUBLIC_LINKS
    vs::vs_core
    vs::vs_kernel
  PRIVATE_LINKS
    vs::planegcs
    nlohmann_json::nlohmann_json)

vs_add_library_tests(vs_sketch
  SOURCES
    tests/canonical.cpp
    tests/test_module.cpp
    tests/test_profile.cpp
    tests/test_serialize.cpp
    tests/test_sketch.cpp
    tests/test_solver.cpp
  LINKS
    vs::vs_kernel)

if(VS_BUILD_TESTS)
  target_compile_definitions(vs_sketch_tests PRIVATE
    VS_SKETCH_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")

  # Acceptance: no PlaneGCS header appears in vs_sketch's public headers.
  #
  # 1. Each public header compiles on its own without PlaneGCS's or Eigen's
  #    include directories (only vs_sketch's, vs_kernel's and vs_core's).
  file(GLOB vs_sketch_public_headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/vs/sketch/*.hpp")
  set(header_check_sources)
  foreach(header IN LISTS vs_sketch_public_headers)
    get_filename_component(stem "${header}" NAME_WE)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_check/${stem}.cpp")
    file(RELATIVE_PATH rel "${CMAKE_CURRENT_SOURCE_DIR}/include" "${header}")
    file(CONFIGURE OUTPUT "${source}"
      CONTENT "#include \"${rel}\"\n#include \"${rel}\"  // and include guards work\n")
    list(APPEND header_check_sources "${source}")
  endforeach()
  add_library(vs_sketch_header_check OBJECT EXCLUDE_FROM_ALL ${header_check_sources})
  target_include_directories(vs_sketch_header_check PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(vs_sketch_header_check PRIVATE vs::vs_core vs::vs_kernel)
  vs_set_compile_options(vs_sketch_header_check)
  add_test(NAME "vs_sketch: public headers compile without PlaneGCS"
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
            --target vs_sketch_header_check --config $<CONFIG>)
  set_tests_properties("vs_sketch: public headers compile without PlaneGCS" PROPERTIES
    LABELS vs_sketch RUN_SERIAL TRUE)

  # 2. No public header names a PlaneGCS or Eigen header.
  add_test(NAME "vs_sketch: public headers include no PlaneGCS header"
    COMMAND "${CMAKE_COMMAND}"
      -DHEADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_public_headers.cmake")
  set_tests_properties("vs_sketch: public headers include no PlaneGCS header" PROPERTIES
    LABELS vs_sketch)
endif()

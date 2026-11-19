# vs_kernel: the kernel interface and its Open CASCADE (OCCT) implementation.
#
# OCCT is linked privately: no public header includes it, so modules above
# this one never see OCCT types or need its include path. The header check
# below enforces that.

find_package(OpenCASCADE CONFIG REQUIRED)

# The OCCT toolkits vs_kernel uses, bottom up.
set(VS_KERNEL_OCCT_TOOLKITS
  TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim
  TKShHealing TKBO TKBool TKFillet TKXSBase TKDE TKDESTEP TKDEIGES)

vs_add_library(vs_kernel
  SOURCES
    src/check.cpp
    src/demo.cpp
    src/exchange.cpp
    src/module.cpp
    src/operations.cpp
    src/primitives.cpp
    src/queries.cpp
    src/shape.cpp
    src/tolerance_spike.cpp
  PUBLIC_LINKS
    vs::vs_core
  PRIVATE_LINKS
    ${VS_KERNEL_OCCT_TOOLKITS})

vs_add_library_tests(vs_kernel
  SOURCES
    tests/test_check.cpp
    tests/test_exchange.cpp
    tests/test_module.cpp
    tests/test_operations.cpp
    tests/test_primitives.cpp
    tests/test_queries.cpp
    tests/test_shape.cpp
    tests/test_tolerance_spike.cpp)

if(VS_BUILD_TESTS)
  # Acceptance: no OCCT header is reachable from a public header.
  #
  # 1. Each public header is compiled on its own in a target that sees only
  #    vs_kernel's and vs_core's include directories, never OCCT's. The
  #    target is built by the test, not by the default build, so a failure
  #    shows up as a failing test with the compiler's message.
  file(GLOB vs_kernel_public_headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/vs/kernel/*.hpp")
  set(header_check_sources)
  foreach(header IN LISTS vs_kernel_public_headers)
    get_filename_component(stem "${header}" NAME_WE)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_check/${stem}.cpp")
    file(RELATIVE_PATH rel "${CMAKE_CURRENT_SOURCE_DIR}/include" "${header}")
    file(CONFIGURE OUTPUT "${source}"
      CONTENT "#include \"${rel}\"\n#include \"${rel}\"  // and include guards work\n")
    list(APPEND header_check_sources "${source}")
  endforeach()
  add_library(vs_kernel_header_check OBJECT EXCLUDE_FROM_ALL ${header_check_sources})
  target_include_directories(vs_kernel_header_check PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
  # vs_core brings fmt and spdlog, whose include directory is vcpkg's shared
  # one; OCCT's headers live in its "opencascade" subdirectory, which nothing
  # here adds.
  target_link_libraries(vs_kernel_header_check PRIVATE vs::vs_core)
  vs_set_compile_options(vs_kernel_header_check)
  add_test(NAME "vs_kernel: public headers compile without OCCT"
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
            --target vs_kernel_header_check --config $<CONFIG>)
  set_tests_properties("vs_kernel: public headers compile without OCCT" PROPERTIES
    LABELS vs_kernel RUN_SERIAL TRUE)

  # 2. No public header names an OCCT header, even through a path that
  #    happens to resolve (such as <opencascade/gp_Pnt.hxx>).
  add_test(NAME "vs_kernel: public headers include no OCCT header"
    COMMAND "${CMAKE_COMMAND}"
      -DHEADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_public_headers.cmake")
  set_tests_properties("vs_kernel: public headers include no OCCT header" PROPERTIES
    LABELS vs_kernel)
endif()

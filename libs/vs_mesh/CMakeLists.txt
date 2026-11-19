# vs_mesh: mesh types, half-edge checks, the seam library and welding (M7);
# conforming tessellation and mesh I/O follow in M8. See
# include/vs/mesh/*.hpp and docs/decisions/0007.

vs_add_library(vs_mesh
  SOURCES
    src/mesh.cpp
    src/module.cpp
    src/seam.cpp
    src/seam_library.cpp
    src/topology.cpp
    src/triangulate.cpp
    src/weld.cpp
  PUBLIC_LINKS
    vs::vs_core
    vs::vs_kernel
    vs::vs_sketch
    vs::vs_feature
    vs::vs_model
    vs::vs_validate)

vs_add_library_tests(vs_mesh
  SOURCES
    tests/meshers.cpp
    tests/test_module.cpp
    tests/test_seam.cpp
    tests/test_seam_library.cpp
    tests/test_topology.cpp
    tests/test_triangulate.cpp
    tests/test_weld.cpp
    ../vs_model/tests/bracket.cpp
    ../vs_validate/tests/fixtures.cpp)

if(VS_BUILD_TESTS)
  target_include_directories(vs_mesh_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../vs_model/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../vs_validate/tests)
endif()

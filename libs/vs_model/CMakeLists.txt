# vs_model: document, volume tree, boundaries, keep-outs, connection surfaces,
# published outputs, reference resolution, dependency graph, project files.
#
# Built on vs_feature (layout and parametric contents). nlohmann-json stays
# private, and so is the vendored miniz behind .vsproj zip files (src/zip.cpp,
# decision 0006).

find_package(nlohmann_json CONFIG REQUIRED)

vs_add_library(vs_model
  SOURCES
    src/build.cpp
    src/check.cpp
    src/dependency.cpp
    src/document.cpp
    src/generated.cpp
    src/hierarchy.cpp
    src/index.cpp
    src/module.cpp
    src/project.cpp
    src/references.cpp
    src/serialize.cpp
    src/zip.cpp
  PUBLIC_LINKS
    vs::vs_core
    vs::vs_kernel
    vs::vs_sketch
    vs::vs_feature
  PRIVATE_LINKS
    nlohmann_json::nlohmann_json
    vs::miniz)

vs_add_library_tests(vs_model
  SOURCES
    tests/test_build.cpp
    tests/test_curved.cpp
    tests/test_dependency.cpp
    tests/test_design_rules.cpp
    tests/test_document.cpp
    tests/test_encapsulation.cpp
    tests/test_nested.cpp
    tests/test_parallel_layout.cpp
    tests/test_generated.cpp
    tests/test_hardening.cpp
    tests/test_index.cpp
    tests/test_module.cpp
    tests/test_project.cpp
    tests/test_zip.cpp)

if(VS_BUILD_TESTS)
  # The shared test models (bracket.*, curved.*, nested.*, rack.*),
  # compiled once for every suite that uses them (vs_model up to vs_app).
  add_library(vs_model_test_fixtures STATIC
    tests/bracket.cpp
    tests/bracket.hpp
    tests/curved.cpp
    tests/curved.hpp
    tests/nested.cpp
    tests/nested.hpp
    tests/rack.cpp
    tests/rack.hpp)
  target_include_directories(vs_model_test_fixtures PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(vs_model_test_fixtures PUBLIC vs::vs_model Catch2::Catch2)
  vs_set_compile_options(vs_model_test_fixtures)
  vs_register_module_target(vs_model_test_fixtures vs_model)
  target_link_libraries(vs_model_tests PRIVATE vs_model_test_fixtures)

  target_compile_definitions(vs_model_tests PRIVATE
    VS_MODEL_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()

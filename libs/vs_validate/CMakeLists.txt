# vs_validate: layout checks (overlap, undeclared contact, connection type
# rules), containment and diagnostics routing. Built on vs_model; see
# include/vs/validate/*.hpp and docs/decisions/0006.

vs_add_library(vs_validate
  SOURCES
    src/containment.cpp
    src/layout.cpp
    src/module.cpp
    src/registry.cpp
    src/validate.cpp
  PUBLIC_LINKS
    vs::vs_core
    vs::vs_kernel
    vs::vs_sketch
    vs::vs_feature
    vs::vs_model)

vs_add_library_tests(vs_validate
  SOURCES
    tests/fixtures.cpp
    tests/test_containment.cpp
    tests/test_fixtures.cpp
    tests/test_layout.cpp
    tests/test_module.cpp
    tests/test_performance.cpp
    tests/test_registry.cpp)

if(VS_BUILD_TESTS)
  target_compile_definitions(vs_validate_tests PRIVATE
    VS_VALIDATE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()

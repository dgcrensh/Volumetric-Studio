# vs_feature: feature tree, reference geometry, variables and equations, rebuild.
#
# Built on vs_kernel (solids) and vs_sketch (sketches and profiles). Neither
# OCCT nor PlaneGCS is visible here: both stay behind their wrapper modules.

find_package(nlohmann_json CONFIG REQUIRED)

vs_add_library(vs_feature
  SOURCES
    src/expression.cpp
    src/module.cpp
    src/rebuild.cpp
    src/serialize.cpp
    src/tree.cpp
    src/variables.cpp
  PUBLIC_LINKS
    vs::vs_core
    vs::vs_kernel
    vs::vs_sketch
  PRIVATE_LINKS
    nlohmann_json::nlohmann_json)

vs_add_library_tests(vs_feature
  SOURCES
    tests/parts.cpp
    tests/test_acceptance.cpp
    tests/test_bezier_features.cpp
    tests/test_datums.cpp
    tests/test_design_rules.cpp
    tests/test_expression.cpp
    tests/test_external.cpp
    tests/test_features.cpp
    tests/test_m4_features.cpp
    tests/test_module.cpp
    tests/test_rebuild.cpp
    tests/test_serialize.cpp
    tests/test_variables.cpp)

if(VS_BUILD_TESTS)
  target_compile_definitions(vs_feature_tests PRIVATE
    VS_FEATURE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()

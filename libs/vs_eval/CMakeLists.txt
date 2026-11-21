# vs_eval: the evaluation engine (M9): a deterministic thread pool,
# cancellation and progress, cache keys and the contents cache (memory and
# disk), the scheduler with volume states and the preview tier, and file
# imports as static volumes. See include/vs/eval/*.hpp and
# docs/decisions/0009.

find_package(nlohmann_json CONFIG REQUIRED)

vs_add_library(vs_eval
  SOURCES
    src/cache.cpp
    src/engine.cpp
    src/import.cpp
    src/measure.cpp
    src/module.cpp
    src/step_export.cpp
    src/thread_pool.cpp
  PUBLIC_LINKS
    vs::vs_core
    vs::vs_kernel
    vs::vs_sketch
    vs::vs_feature
    vs::vs_model
    vs::vs_validate
    vs::vs_mesh
  PRIVATE_LINKS
    nlohmann_json::nlohmann_json)

vs_add_library_tests(vs_eval
  SOURCES
    tests/scenes.cpp
    tests/test_cache.cpp
    tests/test_engine.cpp
    tests/test_import.cpp
    tests/test_module.cpp
    tests/test_nested.cpp
    tests/test_step_export.cpp
    tests/test_thread_pool.cpp)

if(VS_BUILD_TESTS)
  target_link_libraries(vs_eval_tests PRIVATE vs_model_test_fixtures)
  target_compile_definitions(vs_eval_tests PRIVATE
    VS_MESH_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../vs_mesh/tests/data")
endif()

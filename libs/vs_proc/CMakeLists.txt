# vs_proc: procedural node graphs (M10: graph model, type system, node
# library, fields and sampling, the engine's procedural generator; M11:
# field-to-mesh against seams; M12 adds the rest of the node library and
# baking). See include/vs/proc/*.hpp and docs/decisions/0010 and 0011.

find_package(nlohmann_json CONFIG REQUIRED)
# Mesh booleans (M12), linked privately: Manifold's types stay in src/.
find_package(manifold CONFIG REQUIRED)

vs_add_library(vs_proc
  SOURCES
    src/check.cpp
    src/distance.cpp
    src/evaluate.cpp
    src/field.cpp
    src/generator.cpp
    src/graph.cpp
    src/bake.cpp
    src/inputs.cpp
    src/inspect.cpp
    src/mesh_boolean.cpp
    src/mesher.cpp
    src/surface_nets_reference.cpp
    src/module.cpp
    src/nodes.cpp
    src/nodes_children.cpp
    src/nodes_frames.cpp
    src/nodes_infill.cpp
    src/nodes_mesh.cpp
    src/templates.cpp
    src/types.cpp
  # What the public headers include; the rest is private.
  PUBLIC_LINKS
    vs::vs_core
    vs::vs_model
    vs::vs_mesh
    vs::vs_eval
  PRIVATE_LINKS
    vs::vs_kernel
    vs::vs_feature
    nlohmann_json::nlohmann_json
    manifold::manifold)

vs_add_library_tests(vs_proc
  SOURCES
    tests/scenes.cpp
    tests/test_acceptance.cpp
    tests/test_check.cpp
    tests/test_curved.cpp
    tests/test_nested.cpp
    tests/test_children.cpp
    tests/test_configurations.cpp
    tests/test_data.cpp
    tests/test_distance.cpp
    tests/test_distance_golden.cpp
    tests/test_engine.cpp
    tests/test_field.cpp
    tests/test_graph.cpp
    tests/test_mesh_acceptance.cpp
    tests/test_mesher.cpp
    tests/test_parallel_mesher.cpp
    tests/test_module.cpp
    tests/test_nodes.cpp
    tests/test_templates.cpp
    tests/test_infill.cpp
    tests/test_voronoi.cpp
    tests/test_frames.cpp
    tests/test_mesh_nodes.cpp
    tests/test_bake.cpp
    tests/test_mvp_workflows.cpp
    tests/test_memory_and_cancel.cpp)

if(VS_BUILD_TESTS)
  target_link_libraries(vs_proc_tests PRIVATE vs_model_test_fixtures)
  target_compile_definitions(vs_proc_tests PRIVATE
    VS_PROC_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")

  # Manifold is linked privately (plan H1): no public header names one of
  # its headers, as for OCCT in vs_kernel and PlaneGCS in vs_sketch.
  add_test(NAME "vs_proc: public headers include no Manifold header"
    COMMAND "${CMAKE_COMMAND}"
      -DHEADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_public_headers.cmake")
  set_tests_properties("vs_proc: public headers include no Manifold header" PROPERTIES
    LABELS vs_proc)
endif()

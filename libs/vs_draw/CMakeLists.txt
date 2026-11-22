# vs_draw: drawings (L6). Views of an evaluation projected with hidden lines
# removed, dimensions measured from stable or disposable anchors, sheets
# written as SVG, PDF and DXF. See include/vs/draw/*.hpp and
# docs/decisions/0038.

vs_add_library(vs_draw
  SOURCES
    src/drawing.cpp
    src/module.cpp
    src/sheet.cpp
    src/view.cpp
  # What the public headers include.
  PUBLIC_LINKS
    vs::vs_core
    vs::vs_kernel
    vs::vs_model
    vs::vs_validate
    vs::vs_mesh
    vs::vs_eval)

vs_add_library_tests(vs_draw
  SOURCES
    tests/test_drawing.cpp
    tests/test_sheet.cpp
    tests/test_view.cpp)

if(VS_BUILD_TESTS)
  target_link_libraries(vs_draw_tests PRIVATE vs::vs_proc vs_model_test_fixtures)
  target_compile_definitions(vs_draw_tests PRIVATE
    VS_PROC_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../vs_proc/tests/data"
    VS_MODEL_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../vs_model/tests/data")
endif()

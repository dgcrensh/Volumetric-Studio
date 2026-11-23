# vs_app: the Qt desktop application (M13 onward), in two static libraries
# (plan H1):
#
# - vs_app_core: the app's rules without Qt or OCCT's visualisation (scene
#   data, outline, authoring commands and history, sketch editing rules,
#   picking, dimension drags, the node editor's rules, export and shipping
#   checks). Built on every platform, also with VS_BUILD_APP=OFF, and tested
#   by vs_app_core_tests on every CI job.
# - vs_app_ui: the Qt widgets, the OCCT viewport and the session thread,
#   plus main() as vs_app. Built only with -DVS_BUILD_APP=ON (or the
#   windows-msvc-app preset), which also turns on the vcpkg "app" feature to
#   install Qt. Windows only: the viewport embeds OCCT's OpenGL viewer in a
#   native window (WNT_Window). CI doesn't build it.
#
# Every target here is registered under the vs_app module: as an
# application, it may link every library.

add_library(vs_app_core STATIC
  src/authoring.cpp
  src/authoring.hpp
  src/configurations.cpp
  src/configurations.hpp
  src/drawings.cpp
  src/drawings.hpp
  src/dimension_drag.cpp
  src/dimension_drag.hpp
  src/exporter.cpp
  src/exporter.hpp
  src/graph_edit.cpp
  src/graph_edit.hpp
  src/guidance.cpp
  src/guidance.hpp
  src/history.cpp
  src/history.hpp
  src/icons.cpp
  src/icons.hpp
  src/node_preview.cpp
  src/node_preview.hpp
  src/outline.cpp
  src/outline.hpp
  src/picking.cpp
  src/picking.hpp
  src/reference_choices.cpp
  src/reference_choices.hpp
  src/scene.cpp
  src/scene.hpp
  src/ship.cpp
  src/ship.hpp
  src/sketch_editor.cpp
  src/sketch_editor.hpp
  src/tool_catalog.cpp
  src/tool_catalog.hpp)
target_include_directories(vs_app_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(vs_app_core PUBLIC cxx_std_20)
target_link_libraries(vs_app_core
  PUBLIC
    vs::vs_core
    vs::vs_kernel
    vs::vs_sketch
    vs::vs_feature
    vs::vs_model
    vs::vs_validate
    vs::vs_mesh
    vs::vs_eval
    vs::vs_proc
    vs::vs_draw)
vs_set_compile_options(vs_app_core)
vs_register_module_target(vs_app_core vs_app)

if(VS_BUILD_TESTS)
  # The suites of the Qt-free files. samples.* is shared with vs_app_tests.
  add_executable(vs_app_core_tests
    tests/samples.cpp
    tests/samples.hpp
    tests/test_authoring.cpp
    tests/test_configurations.cpp
    tests/test_drawings.cpp
    tests/test_export.cpp
    tests/test_history.cpp
    tests/test_interactive.cpp
    tests/test_outline.cpp
    tests/test_scene.cpp
    tests/test_ship.cpp
    tests/test_sketch_editor.cpp
    tests/test_tool_catalog.cpp)
  target_link_libraries(vs_app_core_tests PRIVATE vs_app_core Catch2::Catch2WithMain)
  target_compile_definitions(vs_app_core_tests PRIVATE
    VS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  vs_set_compile_options(vs_app_core_tests)
  vs_register_module_target(vs_app_core_tests vs_app)
  catch_discover_tests(vs_app_core_tests
    TEST_PREFIX "vs_app_core: "
    PROPERTIES LABELS "vs_app_core")
endif()

if(NOT VS_BUILD_APP)
  return()
endif()

find_package(Qt6 CONFIG REQUIRED COMPONENTS Widgets)
find_package(OpenCASCADE CONFIG REQUIRED)

# Everything but main() is vs_app_ui, so the Catch2 suite (vs_app_tests)
# drives the same code the application runs.
add_library(vs_app_ui STATIC
  src/actions.cpp
  src/actions.hpp
  src/browser.cpp
  src/browser.hpp
  src/problems.cpp
  src/problems.hpp
  src/context_bar.cpp
  src/context_bar.hpp
  src/design_table_dialog.cpp
  src/design_table_dialog.hpp
  src/drawing_dialogs.cpp
  src/drawing_dialogs.hpp
  src/drawing_window.cpp
  src/drawing_window.hpp
  src/feature_dialog.cpp
  src/feature_dialog.hpp
  src/icon_painter.cpp
  src/icon_painter.hpp
  src/main_window.cpp
  src/main_window.hpp
  src/model_dialogs.cpp
  src/model_dialogs.hpp
  src/node_editor.cpp
  src/node_editor.hpp
  src/session.cpp
  src/session.hpp
  src/sketch_mode.cpp
  src/sketch_mode.hpp
  src/viewport.cpp
  src/viewport.hpp
  src/widgets.cpp
  src/widgets.hpp)
set_target_properties(vs_app_ui PROPERTIES AUTOMOC ON)
target_link_libraries(vs_app_ui
  PUBLIC
    vs_app_core
    vs::vs_gpu
    Qt6::Widgets
  PRIVATE
    # OCCT's visualisation toolkits, for the viewport only; the kernel's
    # modelling toolkits stay private to vs_kernel.
    TKOpenGl TKV3d TKService)
vs_set_compile_options(vs_app_ui)
vs_register_module_target(vs_app_ui vs_app)

vs_add_application(vs_app
  SOURCES
    src/main.cpp
  LINKS
    vs_app_ui)
set_target_properties(vs_app PROPERTIES WIN32_EXECUTABLE ON)

if(VS_BUILD_TESTS)
  add_executable(vs_app_tests
    tests/app_test_main.cpp
    tests/samples.cpp
    tests/samples.hpp
    tests/wait.cpp
    tests/wait.hpp
    tests/test_authoring_ui.cpp
    tests/test_configurations_ui.cpp
    tests/test_browser_ui.cpp
    tests/test_context_bar_ui.cpp
    tests/test_dialogs_ui.cpp
    tests/test_drawings_ui.cpp
    tests/test_interactive_ui.cpp
    tests/test_nested.cpp
    tests/test_generated.cpp
    tests/test_node_editor.cpp
    tests/test_problems_ui.cpp
    tests/test_session.cpp
    tests/test_viewport_ui.cpp
    tests/test_ship_ui.cpp
    tests/test_window.cpp)
  target_link_libraries(vs_app_tests PRIVATE vs_app_ui vs_model_test_fixtures Catch2::Catch2)
  target_compile_definitions(vs_app_tests PRIVATE
    VS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  vs_set_compile_options(vs_app_tests)
  vs_register_module_target(vs_app_tests vs_app)
  # Tests tagged [gui] open windows and render with OpenGL: they need a
  # desktop session and a GPU driver, as on a developer machine.
  catch_discover_tests(vs_app_tests
    TEST_PREFIX "vs_app: "
    PROPERTIES LABELS "vs_app")
endif()

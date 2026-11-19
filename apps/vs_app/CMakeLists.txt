# vs_app: the Qt desktop application (M13 onward). Built only with
# -DVS_BUILD_APP=ON (or the windows-msvc-app preset), which also turns on the
# vcpkg "app" feature to install Qt. Windows only: the viewport embeds OCCT's
# OpenGL viewer in a native window (WNT_Window).
#
# Everything but main() is the static library vs_app_lib, so the Catch2
# suite (vs_app_tests) drives the same code the application runs. Both are
# registered under the vs_app module: as an application, it may link every
# library.

find_package(Qt6 CONFIG REQUIRED COMPONENTS Widgets)
find_package(OpenCASCADE CONFIG REQUIRED)

add_library(vs_app_lib STATIC
  src/authoring.cpp
  src/authoring.hpp
  src/exporter.cpp
  src/exporter.hpp
  src/feature_dialog.cpp
  src/feature_dialog.hpp
  src/history.cpp
  src/history.hpp
  src/main_window.cpp
  src/main_window.hpp
  src/model_dialogs.cpp
  src/model_dialogs.hpp
  src/outline.cpp
  src/outline.hpp
  src/reference_choices.cpp
  src/reference_choices.hpp
  src/scene.cpp
  src/scene.hpp
  src/session.cpp
  src/session.hpp
  src/sketch_editor.cpp
  src/sketch_editor.hpp
  src/sketch_mode.cpp
  src/sketch_mode.hpp
  src/viewport.cpp
  src/viewport.hpp
  src/widgets.cpp
  src/widgets.hpp)
set_target_properties(vs_app_lib PROPERTIES AUTOMOC ON)
target_include_directories(vs_app_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(vs_app_lib PUBLIC cxx_std_20)
target_link_libraries(vs_app_lib
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
    Qt6::Widgets
  PRIVATE
    # OCCT's visualisation toolkits, for the viewport only; the kernel's
    # modelling toolkits stay private to vs_kernel.
    TKOpenGl TKV3d TKService)
vs_set_compile_options(vs_app_lib)
vs_register_module_target(vs_app_lib vs_app)

vs_add_application(vs_app
  SOURCES
    src/main.cpp
  LINKS
    vs_app_lib)
set_target_properties(vs_app PROPERTIES WIN32_EXECUTABLE ON)

if(VS_BUILD_TESTS)
  add_executable(vs_app_tests
    tests/app_test_main.cpp
    tests/samples.cpp
    tests/samples.hpp
    tests/test_authoring.cpp
    tests/test_authoring_ui.cpp
    tests/test_export.cpp
    tests/test_history.cpp
    tests/test_outline.cpp
    tests/test_scene.cpp
    tests/test_session.cpp
    tests/test_sketch_editor.cpp
    tests/test_window.cpp)
  target_link_libraries(vs_app_tests PRIVATE vs_app_lib Catch2::Catch2)
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

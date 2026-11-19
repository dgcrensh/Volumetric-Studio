cmake_minimum_required(VERSION 3.25)

# Options that select vcpkg manifest features must be set before project(),
# because vcpkg installs dependencies while the first language is enabled.
option(VS_BUILD_APP "Build the Qt desktop application (vs_app); needs Qt 6" OFF)
option(VS_BUILD_TESTS "Build the Catch2 test suites" ON)
option(VS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# vs_kernel wraps Open CASCADE from M1 onward, so every build needs it.
list(APPEND VCPKG_MANIFEST_FEATURES "kernel")
# vs_sketch vendors PlaneGCS, which needs Eigen, from M2 onward.
list(APPEND VCPKG_MANIFEST_FEATURES "sketch")
if(VS_BUILD_APP)
  list(APPEND VCPKG_MANIFEST_FEATURES "app")
endif()

project(VolumetricStudio
  VERSION 0.1.0
  DESCRIPTION "Volumetric CAD Studio"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(VsCompileOptions)
include(VsModuleRules)
include(VsModules)

if(VS_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)
  include(Catch)
endif()

# Vendored third-party code. These are not modules, so the dependency rule
# doesn't judge them; each is linked privately by its one wrapper module.
add_subdirectory(third_party/planegcs)
add_subdirectory(third_party/miniz)

# Libraries, bottom up. The order here mirrors VS_MODULE_ORDER in
# cmake/VsModuleRules.cmake; a module may link only modules listed before it.
add_subdirectory(libs/vs_core)
add_subdirectory(libs/vs_kernel)
add_subdirectory(libs/vs_sketch)
add_subdirectory(libs/vs_feature)
add_subdirectory(libs/vs_model)
add_subdirectory(libs/vs_validate)
add_subdirectory(libs/vs_mesh)
add_subdirectory(libs/vs_eval)
add_subdirectory(libs/vs_proc)

if(VS_BUILD_APP)
  add_subdirectory(apps/vs_app)
endif()
add_subdirectory(apps/vstudio-cli)

if(VS_BUILD_TESTS)
  add_subdirectory(tests/cmake)
endif()

# Fails configuration if any module links a module it is not allowed to.
vs_verify_module_graph()

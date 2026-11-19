# PlaneGCS: FreeCAD's 2D geometric constraint solver, vendored from
# FreeCAD 1.1.4 (src/Mod/Sketcher/App/planegcs). See README.md for what was
# changed and how to update it.
#
# Only vs_sketch links this target, privately, and no vs_sketch public header
# includes it (checked by ctest).

find_package(Eigen3 CONFIG REQUIRED)

add_library(planegcs STATIC
  src/Constraints.cpp
  src/GCS.cpp
  src/Geo.cpp
  src/SubSystem.cpp
  src/qp_eq.cpp)
add_library(vs::planegcs ALIAS planegcs)

# Vendored headers are SYSTEM for consumers, so vs_sketch's -Werror warnings
# (-Wshadow, -Wsign-conversion) don't fire inside PlaneGCS.
target_include_directories(planegcs
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(planegcs SYSTEM
  INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/shim")
target_link_libraries(planegcs PUBLIC Eigen3::Eigen)
target_compile_features(planegcs PUBLIC cxx_std_20)
# Eigen assertions trip on PlaneGCS's sparse QR use (FreeCAD sets this too).
target_compile_definitions(planegcs PUBLIC EIGEN_NO_DEBUG)

# Third-party code: no warnings, and never warnings as errors. Floating-point
# contraction stays off, as in first-party code, so solves are deterministic.
if(MSVC)
  target_compile_options(planegcs PRIVATE /W0 /permissive- /utf-8 /fp:precise /bigobj)
  target_compile_definitions(planegcs PRIVATE NOMINMAX _USE_MATH_DEFINES)
else()
  target_compile_options(planegcs PRIVATE -w -ffp-contract=off -fno-fast-math)
endif()
set_target_properties(planegcs PROPERTIES POSITION_INDEPENDENT_CODE ON)

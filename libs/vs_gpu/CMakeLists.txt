# vs_gpu: procedural fields sampled on the GPU (L1). An OpenGL 4.3 compute
# context on a thread of its own, field trees translated to GLSL, and a
# sampling backend vs_proc's sample() uses when the GPU gives exactly the
# CPU's values. See include/vs/gpu/*.hpp and docs/decisions/0018.

vs_add_library(vs_gpu
  SOURCES
    src/device.cpp
    src/gl_context.cpp
  PUBLIC_LINKS
    vs::vs_core
    vs::vs_kernel
    vs::vs_sketch
    vs::vs_feature
    vs::vs_model
    vs::vs_validate
    vs::vs_mesh
    vs::vs_eval
    vs::vs_proc)

if(WIN32)
  target_link_libraries(vs_gpu PRIVATE opengl32 gdi32 user32)
endif()

vs_add_library_tests(vs_gpu
  SOURCES
    tests/test_arithmetic.cpp)

if(VS_BUILD_TESTS)
  target_include_directories(vs_gpu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

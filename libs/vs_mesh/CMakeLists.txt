# vs_mesh: mesh types, seam library, conforming tessellation, welding and mesh I/O.
# Empty until M7 fills it in. Add dependencies on lower modules here; the
# configure-time check rejects any link to a module above this one.

vs_add_library(vs_mesh
  SOURCES
    src/module.cpp
  PUBLIC_LINKS
    vs::vs_core)

vs_add_library_tests(vs_mesh
  SOURCES
    tests/test_module.cpp)

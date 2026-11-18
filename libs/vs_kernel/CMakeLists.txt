# vs_kernel: kernel interface and its OCCT implementation (solids, booleans, STEP/IGES).
# Empty until M1 fills it in. Add dependencies on lower modules here; the
# configure-time check rejects any link to a module above this one.

vs_add_library(vs_kernel
  SOURCES
    src/module.cpp
  PUBLIC_LINKS
    vs::vs_core)

vs_add_library_tests(vs_kernel
  SOURCES
    tests/test_module.cpp)

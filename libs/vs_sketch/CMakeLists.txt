# vs_sketch: sketch entities, constraints, the PlaneGCS wrapper and profile extraction.
# Empty until M2 fills it in. Add dependencies on lower modules here; the
# configure-time check rejects any link to a module above this one.

vs_add_library(vs_sketch
  SOURCES
    src/module.cpp
  PUBLIC_LINKS
    vs::vs_core)

vs_add_library_tests(vs_sketch
  SOURCES
    tests/test_module.cpp)

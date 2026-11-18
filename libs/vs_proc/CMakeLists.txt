# vs_proc: procedural node graph, type system, node library, fields and baking.
# Empty until M10 fills it in. Add dependencies on lower modules here; the
# configure-time check rejects any link to a module above this one.

vs_add_library(vs_proc
  SOURCES
    src/module.cpp
  PUBLIC_LINKS
    vs::vs_core)

vs_add_library_tests(vs_proc
  SOURCES
    tests/test_module.cpp)

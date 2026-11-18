# vs_model: document, volume tree, connection surfaces, reference resolution, project files.
# Empty until M5 fills it in. Add dependencies on lower modules here; the
# configure-time check rejects any link to a module above this one.

vs_add_library(vs_model
  SOURCES
    src/module.cpp
  PUBLIC_LINKS
    vs::vs_core)

vs_add_library_tests(vs_model
  SOURCES
    tests/test_module.cpp)

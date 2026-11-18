# vs_validate: layout checks, containment and diagnostics routing.
# Empty until M6 fills it in. Add dependencies on lower modules here; the
# configure-time check rejects any link to a module above this one.

vs_add_library(vs_validate
  SOURCES
    src/module.cpp
  PUBLIC_LINKS
    vs::vs_core)

vs_add_library_tests(vs_validate
  SOURCES
    tests/test_module.cpp)

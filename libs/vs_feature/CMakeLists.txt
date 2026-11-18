# vs_feature: feature tree, reference geometry, variables and equations, rebuild.
# Empty until M3 fills it in. Add dependencies on lower modules here; the
# configure-time check rejects any link to a module above this one.

vs_add_library(vs_feature
  SOURCES
    src/module.cpp
  PUBLIC_LINKS
    vs::vs_core)

vs_add_library_tests(vs_feature
  SOURCES
    tests/test_module.cpp)

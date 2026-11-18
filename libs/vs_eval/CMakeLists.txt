# vs_eval: scheduler, cache, thread pool, cancellation and volume states.
# Empty until M9 fills it in. Add dependencies on lower modules here; the
# configure-time check rejects any link to a module above this one.

vs_add_library(vs_eval
  SOURCES
    src/module.cpp
  PUBLIC_LINKS
    vs::vs_core)

vs_add_library_tests(vs_eval
  SOURCES
    tests/test_module.cpp)

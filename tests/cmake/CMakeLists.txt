# Tests of the build system itself.

# The rule as a pure function: allowed and forbidden pairs.
add_test(NAME "build: module dependency rule"
  COMMAND "${CMAKE_COMMAND}"
    -DVS_RULES_FILE=${PROJECT_SOURCE_DIR}/cmake/VsModuleRules.cmake
    -P "${CMAKE_CURRENT_SOURCE_DIR}/dependency_rule_test.cmake")

# A real configure of a small project with a forbidden link must fail with
# the rule's message; the allowed variant must succeed.
foreach(case IN ITEMS forbidden allowed)
  add_test(NAME "build: configure with ${case} link"
    COMMAND "${CMAKE_COMMAND}"
      -S "${CMAKE_CURRENT_SOURCE_DIR}/fixture"
      -B "${CMAKE_CURRENT_BINARY_DIR}/fixture-${case}"
      -G "${CMAKE_GENERATOR}"
      -DVS_RULES_FILE=${PROJECT_SOURCE_DIR}/cmake/VsModuleRules.cmake
      -DFIXTURE_CASE=${case})
endforeach()
set_tests_properties("build: configure with forbidden link" PROPERTIES
  PASS_REGULAR_EXPRESSION "Forbidden module dependency.*vs_core -> vs_kernel")
set_tests_properties("build: configure with allowed link" PROPERTIES
  PASS_REGULAR_EXPRESSION "Configuring done"
  FAIL_REGULAR_EXPRESSION "Forbidden module dependency")

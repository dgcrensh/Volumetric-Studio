# A tiny project that uses the real dependency check on interface targets, so
# it configures without a compiler. FIXTURE_CASE picks a forbidden or an
# allowed link.

cmake_minimum_required(VERSION 3.25)
project(vs_dependency_fixture LANGUAGES NONE)

include("${VS_RULES_FILE}")

add_library(vs_core INTERFACE)
add_library(vs_kernel INTERFACE)
vs_register_module_target(vs_core vs_core)
vs_register_module_target(vs_kernel vs_kernel)

if(FIXTURE_CASE STREQUAL "forbidden")
  # vs_core sits below vs_kernel, so it may not link it.
  target_link_libraries(vs_core INTERFACE vs_kernel)
elseif(FIXTURE_CASE STREQUAL "allowed")
  target_link_libraries(vs_kernel INTERFACE vs_core)
else()
  message(FATAL_ERROR "FIXTURE_CASE must be 'forbidden' or 'allowed'")
endif()

vs_verify_module_graph()

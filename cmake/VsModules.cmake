# Helpers that every module's CMakeLists.txt uses, so each library gets the
# same layout, compile options and dependency checking.
#
# Layout of a library module:
#   libs/<name>/include/vs/<short>/...   public headers
#   libs/<name>/src/...                  sources and private headers
#   libs/<name>/tests/...                Catch2 tests

include_guard(GLOBAL)

# vs_add_library(<name>
#   SOURCES <src>...
#   [PUBLIC_LINKS <target>...]
#   [PRIVATE_LINKS <target>...])
function(vs_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_LINKS;PRIVATE_LINKS" ${ARGN})
  list(FIND VS_LIBRARY_ORDER "${name}" index)
  if(index EQUAL -1)
    message(FATAL_ERROR "vs_add_library: '${name}' is not listed in VS_LIBRARY_ORDER")
  endif()

  add_library(${name} STATIC ${ARG_SOURCES})
  add_library(vs::${name} ALIAS ${name})
  target_include_directories(${name}
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(${name} PUBLIC cxx_std_20)
  if(ARG_PUBLIC_LINKS)
    target_link_libraries(${name} PUBLIC ${ARG_PUBLIC_LINKS})
  endif()
  if(ARG_PRIVATE_LINKS)
    target_link_libraries(${name} PRIVATE ${ARG_PRIVATE_LINKS})
  endif()
  vs_set_compile_options(${name})
  vs_register_module_target(${name} ${name})
endfunction()

# vs_add_library_tests(<module>
#   SOURCES <src>...
#   [LINKS <target>...])
#
# Builds <module>_tests and registers each Catch2 test case with CTest.
function(vs_add_library_tests module)
  if(NOT VS_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(ARG "" "" "SOURCES;LINKS" ${ARGN})
  set(target ${module}_tests)
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE ${module} Catch2::Catch2WithMain ${ARG_LINKS})
  vs_set_compile_options(${target})
  vs_register_module_target(${target} ${module})
  catch_discover_tests(${target}
    TEST_PREFIX "${module}: "
    PROPERTIES LABELS "${module}")
endfunction()

# vs_add_application(<name>
#   SOURCES <src>...
#   [LINKS <target>...])
function(vs_add_application name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LINKS" ${ARGN})
  list(FIND VS_APPLICATIONS "${name}" index)
  if(index EQUAL -1)
    message(FATAL_ERROR "vs_add_application: '${name}' is not listed in VS_APPLICATIONS")
  endif()
  add_executable(${name} ${ARG_SOURCES})
  if(ARG_LINKS)
    target_link_libraries(${name} PRIVATE ${ARG_LINKS})
  endif()
  vs_set_compile_options(${name})
  vs_register_module_target(${name} ${name})
endfunction()

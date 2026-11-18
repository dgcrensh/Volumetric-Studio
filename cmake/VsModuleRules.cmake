# The module dependency rule from the implementation plan:
#
#   Libraries are ordered bottom up. A library may depend only on libraries
#   listed before it. Applications (vs_app, vstudio-cli) may depend on any
#   library, but never on each other.
#
# This file holds only the rule itself, with no targets, so that it can be
# included from the main build, from `cmake -P` scripts and from the test
# fixture projects under tests/cmake.

include_guard(GLOBAL)

set(VS_LIBRARY_ORDER
  vs_core
  vs_kernel
  vs_sketch
  vs_feature
  vs_model
  vs_validate
  vs_mesh
  vs_eval
  vs_proc)

set(VS_APPLICATIONS
  vs_app
  vstudio-cli)

# vs_module_dependency_allowed(<consumer> <dependency> <out_var> <out_reason>)
#
# Sets <out_var> to TRUE when <consumer> may link <dependency>. Names that are
# not Volumetric Studio modules (third-party targets, system libraries) are not
# judged here and are always allowed.
function(vs_module_dependency_allowed consumer dependency out_var out_reason)
  set(allowed TRUE)
  set(reason "")

  list(FIND VS_LIBRARY_ORDER "${dependency}" dep_index)
  list(FIND VS_APPLICATIONS "${dependency}" dep_app_index)
  list(FIND VS_LIBRARY_ORDER "${consumer}" con_index)
  list(FIND VS_APPLICATIONS "${consumer}" con_app_index)

  if(dep_index EQUAL -1 AND dep_app_index EQUAL -1)
    # Not one of ours.
  elseif(consumer STREQUAL dependency)
    set(allowed FALSE)
    set(reason "a module cannot link itself")
  elseif(NOT dep_app_index EQUAL -1)
    set(allowed FALSE)
    set(reason "applications are not libraries and cannot be linked")
  elseif(NOT con_app_index EQUAL -1)
    # Applications may use every library.
  elseif(con_index EQUAL -1)
    set(allowed FALSE)
    set(reason "'${consumer}' is not a known module")
  elseif(dep_index GREATER con_index)
    set(allowed FALSE)
    set(reason "'${dependency}' sits above '${consumer}' in the module order")
  endif()

  set(${out_var} ${allowed} PARENT_SCOPE)
  set(${out_reason} "${reason}" PARENT_SCOPE)
endfunction()

# Strips generator expressions such as $<LINK_ONLY:x> or $<BUILD_INTERFACE:x>
# down to the target names they wrap, and vs:: aliases down to the module
# name, so they are checked like plain names.
function(_vs_strip_genex item out_var)
  set(name "${item}")
  while(name MATCHES "^\\$<[A-Za-z_]+:(.*)>$")
    set(name "${CMAKE_MATCH_1}")
  endwhile()
  # Our libraries also have vs::<name> aliases.
  if(name MATCHES "^vs::(.+)$")
    set(name "${CMAKE_MATCH_1}")
  endif()
  set(${out_var} "${name}" PARENT_SCOPE)
endfunction()

# vs_register_module_target(<target> <module>)
#
# Marks <target> as belonging to <module> so its links are checked against
# that module's rule. Test executables register under the module they test.
function(vs_register_module_target target module)
  set_property(TARGET ${target} PROPERTY VS_MODULE "${module}")
  set_property(GLOBAL APPEND PROPERTY VS_CHECKED_TARGETS ${target})
endfunction()

# vs_verify_module_graph()
#
# Walks every registered target and fails configuration on a forbidden link.
# Call it once, at the end of the top-level CMakeLists.txt.
function(vs_verify_module_graph)
  get_property(targets GLOBAL PROPERTY VS_CHECKED_TARGETS)
  set(errors "")
  foreach(target IN LISTS targets)
    get_target_property(module ${target} VS_MODULE)
    get_target_property(type ${target} TYPE)
    set(links "")
    if(NOT type STREQUAL "INTERFACE_LIBRARY")
      get_target_property(direct ${target} LINK_LIBRARIES)
      if(direct)
        list(APPEND links ${direct})
      endif()
    endif()
    get_target_property(iface ${target} INTERFACE_LINK_LIBRARIES)
    if(iface)
      list(APPEND links ${iface})
    endif()
    list(REMOVE_DUPLICATES links)
    foreach(item IN LISTS links)
      _vs_strip_genex("${item}" dep)
      # A test target may link the module it tests.
      if(dep STREQUAL module AND NOT target STREQUAL module)
        continue()
      endif()
      vs_module_dependency_allowed("${module}" "${dep}" ok reason)
      if(NOT ok)
        string(APPEND errors "  ${target} -> ${dep}: ${reason}\n")
      endif()
    endforeach()
  endforeach()
  if(errors)
    message(FATAL_ERROR
      "Forbidden module dependency (see 'Architecture at a glance' in "
      "Implementation Plan.md):\n${errors}")
  endif()
endfunction()

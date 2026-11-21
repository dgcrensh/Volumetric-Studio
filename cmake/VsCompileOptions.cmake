# Compiler settings shared by every first-party target.
#
# Determinism matters more than the last few percent of speed (spec, design
# rule 7), so floating-point contraction into FMA is disabled everywhere:
# GCC and Clang get -ffp-contract=off, and MSVC uses /fp:precise, which since
# Visual Studio 2022 does not contract unless /fp:contract is given. Results
# must not depend on the compiler's choice to fuse a multiply and an add.

function(vs_set_compile_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor
      /fp:precise
      /external:anglebrackets /external:W0
      /bigobj)
    target_compile_definitions(${target} PRIVATE
      NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
    if(VS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
      -ffp-contract=off -fno-fast-math)
    if(VS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# vs_enable_sanitizers(<list>)
#
# Builds everything in this project (first-party and vendored code, not the
# vcpkg dependencies) with the given sanitizers, such as "address;undefined"
# (the linux-clang-asan preset, plan H1). Empty: nothing.
#
# UBSan's checks stay recoverable so that scripts/sanitizers/ubsan.supp can
# silence reports inside third-party headers; the preset's
# UBSAN_OPTIONS=halt_on_error=1 still stops the program on any other report,
# so a test can't pass with one in its output.
function(vs_enable_sanitizers sanitizers)
  if(NOT sanitizers)
    return()
  endif()
  if(MSVC)
    message(FATAL_ERROR "VS_SANITIZE is for GCC and Clang builds")
  endif()
  list(JOIN sanitizers "," list)
  set(flags "-fsanitize=${list}" -fno-omit-frame-pointer)
  add_compile_options(${flags})
  add_link_options(${flags})
  message(STATUS "Sanitizers: ${list}")
endfunction()

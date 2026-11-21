# Fails when a vs_proc public header includes a Manifold header.
#
# Manifold's headers live under a "manifold/" prefix (<manifold/manifold.h>)
# and are .h files; first-party headers are .hpp under "vs/". Usage:
#   cmake -DHEADER_DIR=<libs/vs_proc/include> -P check_public_headers.cmake

if(NOT HEADER_DIR)
  message(FATAL_ERROR "HEADER_DIR is not set")
endif()

file(GLOB_RECURSE headers "${HEADER_DIR}/*.hpp" "${HEADER_DIR}/*.h")
if(NOT headers)
  message(FATAL_ERROR "No public headers found under ${HEADER_DIR}")
endif()

set(offences)
foreach(header IN LISTS headers)
  file(STRINGS "${header}" includes REGEX "^[ \t]*#[ \t]*include")
  foreach(line IN LISTS includes)
    if(line MATCHES "[<\"/]manifold/" OR line MATCHES "[<\"]manifold\\.h[\">]")
      file(RELATIVE_PATH rel "${HEADER_DIR}" "${header}")
      list(APPEND offences "${rel}: ${line}")
    endif()
  endforeach()
endforeach()

if(offences)
  list(JOIN offences "\n  " text)
  message(FATAL_ERROR "Manifold headers included from vs_proc public headers:\n  ${text}")
endif()
list(LENGTH headers count)
message(STATUS "${count} public headers checked; none includes Manifold")

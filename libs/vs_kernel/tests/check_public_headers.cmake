# Fails when a vs_kernel public header includes an OCCT header.
#
# OCCT headers end in .hxx/.gxx/.lxx or live under an "opencascade/" prefix;
# first-party headers are .hpp. Usage:
#   cmake -DHEADER_DIR=<libs/vs_kernel/include> -P check_public_headers.cmake

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
    if(line MATCHES "\\.(hxx|gxx|lxx)[\">]" OR line MATCHES "[<\"]opencascade/")
      file(RELATIVE_PATH rel "${HEADER_DIR}" "${header}")
      list(APPEND offences "${rel}: ${line}")
    endif()
  endforeach()
endforeach()

if(offences)
  list(JOIN offences "\n  " text)
  message(FATAL_ERROR "OCCT headers included from vs_kernel public headers:\n  ${text}")
endif()
list(LENGTH headers count)
message(STATUS "${count} public headers checked; none includes OCCT")

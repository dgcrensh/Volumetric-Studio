# Fails when a vs_sketch public header includes a PlaneGCS or Eigen header.
#
# PlaneGCS's headers are GCS.h, Geo.h, Constraints.h, SubSystem.h, Util.h and
# qp_eq.h; Eigen's live under "Eigen/" or "eigen3/". First-party headers are
# .hpp under "vs/". Usage:
#   cmake -DHEADER_DIR=<libs/vs_sketch/include> -P check_public_headers.cmake

if(NOT HEADER_DIR)
  message(FATAL_ERROR "HEADER_DIR is not set")
endif()

file(GLOB_RECURSE headers "${HEADER_DIR}/*.hpp" "${HEADER_DIR}/*.h")
if(NOT headers)
  message(FATAL_ERROR "No public headers found under ${HEADER_DIR}")
endif()

set(planegcs_headers "GCS|Geo|Constraints|SubSystem|Util|qp_eq|SketcherGlobal")
set(offences)
foreach(header IN LISTS headers)
  file(STRINGS "${header}" includes REGEX "^[ \t]*#[ \t]*include")
  foreach(line IN LISTS includes)
    if(line MATCHES "[<\"/](${planegcs_headers})\.h[\">]"
       OR line MATCHES "[<\"](Eigen|eigen3|planegcs)/")
      file(RELATIVE_PATH rel "${HEADER_DIR}" "${header}")
      list(APPEND offences "${rel}: ${line}")
    endif()
  endforeach()
endforeach()

if(offences)
  list(JOIN offences "\n  " text)
  message(FATAL_ERROR "PlaneGCS or Eigen headers included from vs_sketch public headers:\n  ${text}")
endif()
list(LENGTH headers count)
message(STATUS "${count} public headers checked; none includes PlaneGCS or Eigen")

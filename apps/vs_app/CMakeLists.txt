# vs_app: the Qt desktop application. Built only with -DVS_BUILD_APP=ON (or
# the windows-msvc-app preset), which also turns on the vcpkg "app" feature
# to install Qt. Empty until M13 fills it in.

find_package(Qt6 CONFIG REQUIRED COMPONENTS Widgets)

vs_add_application(vs_app
  SOURCES
    src/main.cpp
  LINKS
    vs::vs_core
    Qt6::Widgets)

set_target_properties(vs_app PROPERTIES WIN32_EXECUTABLE ON)

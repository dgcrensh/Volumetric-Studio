# vstudio-cli: the command-line driver over the libraries, used for tests and
# scripting until the GUI milestones. Later milestones add subcommands
# (build, info, validate, export, rebuild).

vs_add_application(vstudio-cli
  SOURCES
    src/feature_commands.cpp
    src/kernel_commands.cpp
    src/main.cpp
    src/model_commands.cpp
    src/sketch_commands.cpp
    src/selftest.cpp
  LINKS
    vs::vs_core
    vs::vs_kernel
    vs::vs_sketch
    vs::vs_feature
    vs::vs_model
    vs::vs_validate
    vs::vs_mesh
    vs::vs_eval
    vs::vs_proc)

if(VS_BUILD_TESTS)
  add_test(NAME "vstudio-cli: --version" COMMAND vstudio-cli --version)
  set_tests_properties("vstudio-cli: --version" PROPERTIES
    PASS_REGULAR_EXPRESSION "^vstudio-cli ${PROJECT_VERSION}")

  add_test(NAME "vstudio-cli: selftest" COMMAND vstudio-cli selftest)

  add_test(NAME "vstudio-cli: unknown command fails" COMMAND vstudio-cli frobnicate)
  set_tests_properties("vstudio-cli: unknown command fails" PROPERTIES WILL_FAIL TRUE)

  add_test(NAME "vstudio-cli: kernel-demo writes STEP"
    COMMAND vstudio-cli kernel-demo "${CMAKE_CURRENT_BINARY_DIR}/kernel-demo.step")
  set_tests_properties("vstudio-cli: kernel-demo writes STEP" PROPERTIES
    PASS_REGULAR_EXPRESSION "valid +yes.*wrote ")
endif()

if(VS_BUILD_TESTS)
  add_test(NAME "vstudio-cli: sketch-solve solves the sample bracket"
    COMMAND vstudio-cli sketch-solve
            "${PROJECT_SOURCE_DIR}/libs/vs_sketch/tests/data/bracket.vsketch.json"
            "${CMAKE_CURRENT_BINARY_DIR}/bracket.solved.json")
  set_tests_properties("vstudio-cli: sketch-solve solves the sample bracket" PROPERTIES
    PASS_REGULAR_EXPRESSION "status +solved.*profiles +1.*1 hole, face valid.*wrote ")
endif()

if(VS_BUILD_TESTS)
  add_test(NAME "vstudio-cli: build rebuilds the sample plate and writes STEP"
    COMMAND vstudio-cli build
            "${PROJECT_SOURCE_DIR}/libs/vs_feature/tests/data/plate.vfeatures.json"
            "${CMAKE_CURRENT_BINARY_DIR}/plate.step")
  set_tests_properties("vstudio-cli: build rebuilds the sample plate and writes STEP" PROPERTIES
    PASS_REGULAR_EXPRESSION "Extrude +ok +Hole.*1 solid, valid yes.*volume +13495\.22[0-9]+ mm.*wrote ")
endif()

if(VS_BUILD_TESTS)
  add_test(NAME "vstudio-cli: info prints the sample bracket project"
    COMMAND vstudio-cli info "${PROJECT_SOURCE_DIR}/libs/vs_model/tests/data/bracket.vsproj"
            --build)
  set_tests_properties("vstudio-cli: info prints the sample bracket project" PROPERTIES
    PASS_REGULAR_EXPRESSION "volumes \\(2\\).*Base +parametric.*Upright +static.*Flange +bolted_flange +joins Base and Upright.*M4 bolts.*links \\(1\\).*Base\\.mass.*Upright\\.load.*link 'Base\\.mass[^\n]*output 'mass' of 'Base'.*Base +current +boundary 24000 mm.*mass = 29\\.76.*Flange +ok +area 600 mm")

  add_test(NAME "vstudio-cli: info fails on a missing project"
    COMMAND vstudio-cli info "${CMAKE_CURRENT_BINARY_DIR}/no-such.vsproj")
  set_tests_properties("vstudio-cli: info fails on a missing project" PROPERTIES WILL_FAIL TRUE)
endif()

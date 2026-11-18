# vstudio-cli: the command-line driver over the libraries, used for tests and
# scripting until the GUI milestones. Later milestones add subcommands
# (kernel-demo, build, info, validate, export, rebuild).

vs_add_application(vstudio-cli
  SOURCES
    src/main.cpp
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
endif()

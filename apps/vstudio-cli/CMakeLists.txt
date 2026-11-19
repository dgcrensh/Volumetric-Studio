# vstudio-cli: the command-line driver over the libraries, used for tests and
# scripting until the GUI milestones. Later milestones add subcommands
# (build, info, validate, export, rebuild).

vs_add_application(vstudio-cli
  SOURCES
    src/feature_commands.cpp
    src/kernel_commands.cpp
    src/main.cpp
    src/mesh_commands.cpp
    src/model_commands.cpp
    src/sketch_commands.cpp
    src/selftest.cpp
    src/validate_commands.cpp
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

if(VS_BUILD_TESTS)
  set(validate_data "${PROJECT_SOURCE_DIR}/libs/vs_validate/tests/data")
  add_test(NAME "vstudio-cli: validate passes a clean project"
    COMMAND vstudio-cli validate "${validate_data}/clean.vsproj")
  set_tests_properties("vstudio-cli: validate passes a clean project" PROPERTIES
    PASS_REGULAR_EXPRESSION "diagnostics 0 errors, 0 warnings.*result +valid")
  add_test(NAME "vstudio-cli: validate passes the sample bracket"
    COMMAND vstudio-cli validate "${PROJECT_SOURCE_DIR}/libs/vs_model/tests/data/bracket.vsproj")
  set_tests_properties("vstudio-cli: validate passes the sample bracket" PROPERTIES
    PASS_REGULAR_EXPRESSION "diagnostics 0 errors, 0 warnings.*result +valid")
  add_test(NAME "vstudio-cli: validate warns about contact without failing"
    COMMAND vstudio-cli validate "${validate_data}/contact.vsproj")
  set_tests_properties("vstudio-cli: validate warns about contact without failing" PROPERTIES
    PASS_REGULAR_EXPRESSION "warning \\[validate\\.contact\\].*act on layout.*valid, with warnings")
  # Each failing fixture exits non-zero (WILL_FAIL) ...
  foreach(fixture overlap mismatch loop containment)
    add_test(NAME "vstudio-cli: validate fails on ${fixture}"
      COMMAND vstudio-cli validate "${validate_data}/${fixture}.vsproj")
    set_tests_properties("vstudio-cli: validate fails on ${fixture}" PROPERTIES WILL_FAIL TRUE)
  endforeach()
  # ... and names the problem and where to act (a pass regex ignores the exit code).
  set(expect_overlap "error \\[validate\\.overlap\\].*overlap by 10 mm.*act on layout")
  set(expect_mismatch "validate\\.connection\\.edge_wall.*act on connection surface: Flange.*validate\\.connection\\.hole_spacing")
  set(expect_loop "error \\[model\\.dependency\\.loop\\].*act on layout")
  set(expect_containment "error \\[validate\\.containment\\].*reach 5 mm outside.*act on contents: Block, in 'Block'")
  foreach(fixture overlap mismatch loop containment)
    add_test(NAME "vstudio-cli: validate explains ${fixture}"
      COMMAND vstudio-cli validate "${validate_data}/${fixture}.vsproj")
    set_tests_properties("vstudio-cli: validate explains ${fixture}" PROPERTIES
      PASS_REGULAR_EXPRESSION "${expect_${fixture}}.*result +invalid")
  endforeach()
endif()

if(VS_BUILD_TESTS)
  add_test(NAME "vstudio-cli: seams prints the sample bracket's flange seam"
    COMMAND vstudio-cli seams "${PROJECT_SOURCE_DIR}/libs/vs_model/tests/data/bracket.vsproj")
  set_tests_properties("vstudio-cli: seams prints the sample bracket's flange seam" PROPERTIES
    PASS_REGULAR_EXPRESSION "seams +1 of 1.*Flange +spacing 3\\.802 mm \\(derived\\), 70 outline.*3 loops \\(2 holes\\).*disc ok.*result +ok")
  add_test(NAME "vstudio-cli: seams fails on a missing project"
    COMMAND vstudio-cli seams "${CMAKE_CURRENT_BINARY_DIR}/no-such.vsproj")
  set_tests_properties("vstudio-cli: seams fails on a missing project" PROPERTIES WILL_FAIL TRUE)
endif()

if(VS_BUILD_TESTS)
  set(bracket_project "${PROJECT_SOURCE_DIR}/libs/vs_model/tests/data/bracket.vsproj")
  add_test(NAME "vstudio-cli: export writes the bracket as one watertight 3MF"
    COMMAND vstudio-cli export "${bracket_project}" --3mf "${CMAKE_CURRENT_BINARY_DIR}/bracket.3mf"
            --stl "${CMAKE_CURRENT_BINARY_DIR}/bracket.stl")
  set_tests_properties("vstudio-cli: export writes the bracket as one watertight 3MF" PROPERTIES
    PASS_REGULAR_EXPRESSION "Base +[0-9]+ triangles, 1 seam.*Upright +[0-9]+ triangles, 1 seam.*combined +[0-9]+ triangles, [0-9]+ vertices, 1 shell, watertight yes, manifold yes, volume 48000 mm.*wrote.*STL.*wrote.*3MF, 1 object.*result +ok")
  add_test(NAME "vstudio-cli: export writes one 3MF object per volume"
    COMMAND vstudio-cli export "${bracket_project}" --per-volume
            --3mf "${CMAKE_CURRENT_BINARY_DIR}/bracket-volumes.3mf")
  set_tests_properties("vstudio-cli: export writes one 3MF object per volume" PROPERTIES
    PASS_REGULAR_EXPRESSION "3MF, 2 objects.*result +ok")
  add_test(NAME "vstudio-cli: export needs a format"
    COMMAND vstudio-cli export "${bracket_project}")
  set_tests_properties("vstudio-cli: export needs a format" PROPERTIES WILL_FAIL TRUE)

  # import, then export what it made: the imported volume is its own shell.
  add_test(NAME "vstudio-cli: import adds a mesh as a static volume"
    COMMAND vstudio-cli import "${bracket_project}"
            "${PROJECT_SOURCE_DIR}/libs/vs_mesh/tests/data/hex_prism.stl" --name Hex
            --out "${CMAKE_CURRENT_BINARY_DIR}/imported.vsproj")
  set_tests_properties("vstudio-cli: import adds a mesh as a static volume" PROPERTIES
    PASS_REGULAR_EXPRESSION "imported +hex_prism.stl as static volume 'Hex'"
    FIXTURES_SETUP imported_project)
  add_test(NAME "vstudio-cli: export includes an imported mesh volume"
    COMMAND vstudio-cli export "${CMAKE_CURRENT_BINARY_DIR}/imported.vsproj"
            --stl "${CMAKE_CURRENT_BINARY_DIR}/imported.stl")
  set_tests_properties("vstudio-cli: export includes an imported mesh volume" PROPERTIES
    PASS_REGULAR_EXPRESSION "Hex +20 triangles, 0 seams.*combined.* 2 shells, watertight yes, manifold yes.*result +ok"
    FIXTURES_REQUIRED imported_project)
endif()


# vstudio-cli: the command-line driver over the libraries, used for tests and
# scripting until the GUI milestones. Later milestones add subcommands
# (build, info, validate, export, import, rebuild).

vs_add_application(vstudio-cli
  SOURCES
    src/eval_commands.cpp
    src/feature_commands.cpp
    src/kernel_commands.cpp
    src/main.cpp
    src/mesh_commands.cpp
    src/model_commands.cpp
    src/proc_commands.cpp
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


if(VS_BUILD_TESTS)
  set(bracket_project "${PROJECT_SOURCE_DIR}/libs/vs_model/tests/data/bracket.vsproj")
  add_test(NAME "vstudio-cli: rebuild evaluates the bracket, then serves it from the cache"
    COMMAND vstudio-cli rebuild "${bracket_project}" --threads 4 --weld --twice)
  set_tests_properties("vstudio-cli: rebuild evaluates the bracket, then serves it from the cache"
    PROPERTIES PASS_REGULAR_EXPRESSION
    "evaluation  1.*Base +current +regenerated.*mass = 29\\.76.*Upright +current +regenerated.*stats +2 regenerated, 0 from cache.*weld .* 1 shell, watertight yes, manifold yes.*evaluation  2.*Base +current +cache.*stats +0 regenerated, 2 from cache.*result +ok")
  add_test(NAME "vstudio-cli: rebuild --set regenerates only what changed"
    COMMAND vstudio-cli rebuild "${bracket_project}" --set Base.thickness=8 --check-determinism)
  set_tests_properties("vstudio-cli: rebuild --set regenerates only what changed" PROPERTIES
    PASS_REGULAR_EXPRESSION "checking determinism.*Base +current +regenerated.*mass = 23\\.808.*result +ok")
  add_test(NAME "vstudio-cli: rebuild fails on contents outside their boundary"
    COMMAND vstudio-cli rebuild "${PROJECT_SOURCE_DIR}/libs/vs_validate/tests/data/containment.vsproj")
  set_tests_properties("vstudio-cli: rebuild fails on contents outside their boundary" PROPERTIES
    PASS_REGULAR_EXPRESSION "Block +failed .*validate\\.containment.*result +errors")
  add_test(NAME "vstudio-cli: rebuild exits 1 on errors"
    COMMAND vstudio-cli rebuild "${PROJECT_SOURCE_DIR}/libs/vs_validate/tests/data/containment.vsproj")
  set_tests_properties("vstudio-cli: rebuild exits 1 on errors" PROPERTIES WILL_FAIL TRUE)
  add_test(NAME "vstudio-cli: rebuild rejects a bad thread count"
    COMMAND vstudio-cli rebuild "${bracket_project}" --threads 0)
  set_tests_properties("vstudio-cli: rebuild rejects a bad thread count" PROPERTIES WILL_FAIL TRUE)
  add_test(NAME "vstudio-cli: import adds a STEP file as a static volume"
    COMMAND vstudio-cli import "${PROJECT_SOURCE_DIR}/libs/vs_validate/tests/data/clean.vsproj"
            "${CMAKE_CURRENT_BINARY_DIR}/kernel-demo.step" --name Demo
            --out "${CMAKE_CURRENT_BINARY_DIR}/imported-step.vsproj")
  set_tests_properties("vstudio-cli: import adds a STEP file as a static volume" PROPERTIES
    PASS_REGULAR_EXPRESSION "imported +kernel-demo.step as static volume 'Demo'"
    DEPENDS "vstudio-cli: kernel-demo writes STEP"
    FIXTURES_SETUP imported_step)
  # The demo part lands on the clean fixture's blocks: its contents evaluate,
  # and the engine's layout check reports the overlap.
  add_test(NAME "vstudio-cli: rebuild evaluates an imported STEP volume"
    COMMAND vstudio-cli rebuild "${CMAKE_CURRENT_BINARY_DIR}/imported-step.vsproj")
  set_tests_properties("vstudio-cli: rebuild evaluates an imported STEP volume" PROPERTIES
    PASS_REGULAR_EXPRESSION "Demo +current +regenerated.*validate\\.overlap\\] 'Base' and 'Demo'.*result +errors"
    FIXTURES_REQUIRED imported_step)
endif()

if(VS_BUILD_TESTS)
  set(proc_data "${PROJECT_SOURCE_DIR}/libs/vs_proc/tests/data")
  add_test(NAME "vstudio-cli: nodes lists the node library" COMMAND vstudio-cli nodes)
  set_tests_properties("vstudio-cli: nodes lists the node library" PROPERTIES
    PASS_REGULAR_EXPRESSION "field\\.shell.*in  field: field, thickness: scalar = 1.*input\\.surface.*property surface.*output")
  add_test(NAME "vstudio-cli: graph checks the sample graph"
    COMMAND vstudio-cli graph "${proc_data}/shell_and_ball.vgraph.json")
  set_tests_properties("vstudio-cli: graph checks the sample graph" PROPERTIES
    PASS_REGULAR_EXPRESSION "graph +6 nodes, 5 edges, 1 parameters.*Shell +field\\.shell.*field <- Boundary\\.field.*thickness <- Wall\\.value.*Output +output.*result +ok")
  add_test(NAME "vstudio-cli: graph rejects a loop"
    COMMAND vstudio-cli graph "${proc_data}/loop.vgraph.json")
  set_tests_properties("vstudio-cli: graph rejects a loop" PROPERTIES
    PASS_REGULAR_EXPRESSION "proc\\.graph\\.loop.*result +errors")
  add_test(NAME "vstudio-cli: graph exits 1 on a loop"
    COMMAND vstudio-cli graph "${proc_data}/loop.vgraph.json")
  set_tests_properties("vstudio-cli: graph exits 1 on a loop" PROPERTIES WILL_FAIL TRUE)
  add_test(NAME "vstudio-cli: rebuild samples a procedural volume"
    COMMAND vstudio-cli rebuild "${proc_data}/shell_and_ball.vsproj" --threads 4 --check-determinism)
  set_tests_properties("vstudio-cli: rebuild samples a procedural volume" PROPERTIES
    PASS_REGULAR_EXPRESSION "Shelled +current +regenerated.*field 41 x 31 x 21 samples, 1 mm apart.*mesh [0-9]+ triangles.*result +ok")
  add_test(NAME "vstudio-cli: rebuild meshes a procedural volume and writes STL"
    COMMAND vstudio-cli rebuild "${proc_data}/shell_and_ball.vsproj" --stl
            "${CMAKE_CURRENT_BINARY_DIR}/shell_and_ball.stl")
  set_tests_properties("vstudio-cli: rebuild meshes a procedural volume and writes STL" PROPERTIES
    PASS_REGULAR_EXPRESSION "weld +[0-9]+ triangles, 3 shells, watertight yes, manifold yes.*wrote .*shell_and_ball\\.stl.*result +ok")

  # M12: templates, outputs, inspection, bake and unbake, the gyroid bench.
  add_test(NAME "vstudio-cli: templates lists the starter templates" COMMAND vstudio-cli templates)
  set_tests_properties("vstudio-cli: templates lists the starter templates" PROPERTIES
    PASS_REGULAR_EXPRESSION "gyroid_infill.*graded_lattice.*bolt_bosses")
  add_test(NAME "vstudio-cli: rebuild generates bolt bosses and publishes their count"
    COMMAND vstudio-cli rebuild "${proc_data}/bolt_bosses.vsproj" --threads 4 --weld)
  set_tests_properties("vstudio-cli: rebuild generates bolt bosses and publishes their count" PROPERTIES
    PASS_REGULAR_EXPRESSION "Bosses +current.*1 seam stitched.*boss_count = 1.*mass = [0-9.]+.*watertight yes, manifold yes.*result +ok")
  add_test(NAME "vstudio-cli: inspect evaluates one node"
    COMMAND vstudio-cli inspect "${proc_data}/bolt_bosses.vsproj" Bosses Flange --preview)
  set_tests_properties("vstudio-cli: inspect evaluates one node" PROPERTIES
    PASS_REGULAR_EXPRESSION "Flange \\(input\\.surface\\), 1 nodes evaluated.*area +100.*holes +1 frames.*preview 1 points")
  add_test(NAME "vstudio-cli: bake writes a static volume"
    COMMAND vstudio-cli bake "${proc_data}/bolt_bosses.vsproj" Bosses
            --out "${CMAKE_CURRENT_BINARY_DIR}/baked.vsproj")
  set_tests_properties("vstudio-cli: bake writes a static volume" PROPERTIES
    PASS_REGULAR_EXPRESSION "baked +content hash" FIXTURES_SETUP baked)
  add_test(NAME "vstudio-cli: a baked volume welds"
    COMMAND vstudio-cli rebuild "${CMAKE_CURRENT_BINARY_DIR}/baked.vsproj" --weld)
  set_tests_properties("vstudio-cli: a baked volume welds" PROPERTIES
    PASS_REGULAR_EXPRESSION "watertight yes, manifold yes.*result +ok" FIXTURES_REQUIRED baked)
  add_test(NAME "vstudio-cli: unbake restores the procedural volume"
    COMMAND vstudio-cli unbake "${CMAKE_CURRENT_BINARY_DIR}/baked.vsproj" Bosses
            --out "${CMAKE_CURRENT_BINARY_DIR}/unbaked.vsproj")
  set_tests_properties("vstudio-cli: unbake restores the procedural volume" PROPERTIES
    PASS_REGULAR_EXPRESSION "unbaked +content hash" FIXTURES_REQUIRED baked)
  add_test(NAME "vstudio-cli: bench-gyroid times a small gyroid"
    COMMAND vstudio-cli bench-gyroid --size 20 --cell 1)
  set_tests_properties("vstudio-cli: bench-gyroid times a small gyroid" PROPERTIES
    PASS_REGULAR_EXPRESSION "surface nets.*containment.*contained.*engine.*mass")
endif()

# Run with: cmake -DVS_RULES_FILE=<path to VsModuleRules.cmake> -P dependency_rule_test.cmake

include("${VS_RULES_FILE}")

set(failures 0)

function(expect consumer dependency expected)
  vs_module_dependency_allowed("${consumer}" "${dependency}" ok reason)
  if(NOT ok STREQUAL expected)
    message(SEND_ERROR "${consumer} -> ${dependency}: expected ${expected}, got ${ok} (${reason})")
  endif()
endfunction()

# Downward links are allowed.
expect(vs_kernel vs_core TRUE)
expect(vs_proc vs_eval TRUE)
expect(vs_proc vs_core TRUE)
expect(vs_mesh vs_model TRUE)

# Upward and self links are forbidden.
expect(vs_core vs_kernel FALSE)
expect(vs_model vs_validate FALSE)
expect(vs_kernel vs_kernel FALSE)
expect(vs_eval vs_proc FALSE)

# Applications may use any library, but not each other, and libraries may
# never link an application.
expect(vstudio-cli vs_proc TRUE)
expect(vs_app vs_core TRUE)
expect(vstudio-cli vs_app FALSE)
expect(vs_core vstudio-cli FALSE)

# Third-party targets are not judged by this rule.
expect(vs_core fmt::fmt TRUE)
expect(vs_core Catch2::Catch2WithMain TRUE)

# Unknown first-party consumers are rejected.
expect(vs_unknown vs_core FALSE)

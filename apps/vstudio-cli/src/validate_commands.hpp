#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `validate <project>`: loads a .vsproj (folder or zip) without refusing a
// document that breaks the authoring rules, runs vs_validate's whole
// validation (check, build, layout checks, containment) and prints every
// diagnostic with the one place to act on it. Returns 0 when there are no
// errors (warnings allowed), 1 when there are, 2 for bad arguments.
int run_validate(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vs::cli

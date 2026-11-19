#pragma once

// vs_feature: feature tree, reference geometry, variables and equations, rebuild.
//
//   expression.hpp  the expression language for variables and dimensions
//   variables.hpp   global variables, evaluated in dependency order, loops rejected
//   tree.hpp        features, references and the feature tree
//   rebuild.hpp     rebuilding a tree into a solid, face naming, diagnostics
//   serialize.hpp   the JSON file format (`vstudio-cli build`)

#include <string_view>

namespace vs::feature {

// The module's name.
std::string_view module_name();

}  // namespace vs::feature

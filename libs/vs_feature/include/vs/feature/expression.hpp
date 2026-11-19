#pragma once

// The expression language for variables, equations and dimensions.
//
// Expressions are small arithmetic formulas such as
//
//   40 mm
//   width / 2 + 3
//   2 * thickness + sqrt(hole_d ^ 2 - 4)
//   atan2(rise, run) - 15 deg
//
// Grammar, loosest binding first:
//
//   expression := term (("+" | "-") term)*
//   term       := unary (("*" | "/") unary)*
//   unary      := ("-" | "+") unary | power
//   power      := primary ("^" unary)?          right-associative; -2^2 is -4
//   primary    := number unit? | name | name "(" args ")" | "(" expression ")"
//
// Numbers are decimal with an optional exponent (1.5, 2e-3). A unit may
// follow a number directly ("10mm", "10 mm"); it converts the number into
// the studio's units, millimetres for lengths and radians for angles. Units:
// mm, cm, m, um, in, ft (lengths) and deg, ° and rad (angles). A number
// without a unit is taken as it is (so already in mm or radians). There is
// no dimensional analysis: "10 mm + 1 deg" is legal and means 10.01745...
//
// Names refer to variables. The constant `pi` and the functions below are
// built in, and so are the unit names; none of them can name a variable.
//
//   sin cos tan (radians)   asin acos atan atan2(y, x)   sqrt abs exp ln log10
//   floor ceil round        pow(x, y) hypot(x, y)        min(...) max(...)
//
// Errors are diagnostics: "feature.expression.syntax" (with the position),
// "feature.expression.unknown_function" and "feature.expression.arity" when
// parsing; "feature.expression.unknown_name" and
// "feature.expression.not_finite" (division by zero, sqrt(-1), ...) when
// evaluating.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vs/core/diagnostic.hpp"

namespace vs::feature {

using core::Diagnostic;
using core::Diagnostics;
using core::Result;

class Expression {
 public:
  // Looks a variable up by name; nullopt when there is no such variable.
  using Lookup = std::function<std::optional<double>(std::string_view name)>;

  // The constant 0.
  Expression();

  static Result<Expression> parse(std::string_view text);

  // The text it was parsed from.
  [[nodiscard]] const std::string& text() const;

  // The variable names it refers to, sorted and without repeats.
  [[nodiscard]] const std::vector<std::string>& references() const;

  // True when it refers to no variables.
  [[nodiscard]] bool is_constant() const { return references().empty(); }

  [[nodiscard]] Result<double> evaluate(const Lookup& lookup) const;

 private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};

// Parses and evaluates in one step.
Result<double> evaluate_expression(std::string_view text, const Expression::Lookup& lookup);

// A legal variable name: a letter or "_" followed by letters, digits or "_",
// at most 64 characters, and not a built-in name.
bool is_valid_name(std::string_view name);

// True for the built-in names (functions, constants and units).
bool is_reserved_name(std::string_view name);

}  // namespace vs::feature

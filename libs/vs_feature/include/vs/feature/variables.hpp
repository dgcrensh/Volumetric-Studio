#pragma once

// Global variables and equations.
//
// A variable is a name and an expression (expression.hpp), which may refer
// to other variables: `width = 40 mm`, `half = width / 2`. Variables are
// evaluated in dependency order, so they can be listed in any order.
//
// Dependencies must not form a loop (spec, design rule 6). Loops are
// rejected when they are authored: add() and set_expression() refuse a
// change that would close one, with "feature.variable.loop" naming the whole
// loop as a path ("a → b → c → a", read "a uses b, which uses c, which uses
// a"). Its subjects are the variables along the loop, starting with the one
// being edited.
//
// Other authoring errors, which also leave the table unchanged:
//   feature.variable.bad_name       not a legal name (see is_valid_name())
//   feature.variable.duplicate_name another variable already has that name
//   feature.variable.unknown        the expression uses a name that isn't a variable
//   feature.expression.*            the expression doesn't parse
//
// Removing a variable is always allowed; expressions that used it then fail
// to evaluate with "feature.variable.unknown".

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/hash.hpp"
#include "vs/core/id.hpp"
#include "vs/feature/expression.hpp"

namespace vs::feature {

using core::Id;

struct Variable {
  Id id;
  std::string name;
  std::string expression;
  friend bool operator==(const Variable&, const Variable&) = default;
};

// The value of every variable that evaluated, by name.
struct VariableValues {
  std::map<std::string, double, std::less<>> values;
  // Why some variables have no value (unknown names, bad expressions, ...).
  Diagnostics diagnostics;

  [[nodiscard]] std::optional<double> get(std::string_view name) const;
  // A lookup for Expression::evaluate().
  [[nodiscard]] Expression::Lookup lookup() const;
  friend bool operator==(const VariableValues& a, const VariableValues& b) {
    return a.values == b.values;
  }
};

class VariableTable {
 public:
  // Adds a variable. Returns its id (random unless given), or the error that
  // kept it out.
  Result<Id> add(std::string name, std::string expression, Id id = {});

  // Changes a variable's expression, unless that would be an error (a loop
  // included). The returned diagnostics are empty on success.
  Diagnostics set_expression(const Id& id, std::string expression);
  Diagnostics set_expression(std::string_view name, std::string expression);

  bool remove(const Id& id);

  [[nodiscard]] const std::vector<Variable>& variables() const { return variables_; }
  [[nodiscard]] const Variable* find(const Id& id) const;
  [[nodiscard]] const Variable* find(std::string_view name) const;
  [[nodiscard]] bool empty() const { return variables_.empty(); }

  // Every problem with the table as it stands: names, expressions, unknown
  // references and loops. Used after reading a file, whose variables arrive
  // without the per-edit checks.
  [[nodiscard]] Diagnostics check() const;

  // Evaluates every variable in dependency order (ties in table order).
  [[nodiscard]] VariableValues evaluate() const;

  // The variables in an order where each comes after those it uses; those
  // caught in a loop are left out.
  [[nodiscard]] std::vector<Id> dependency_order() const;

  [[nodiscard]] core::SubjectRef subject(const Id& id) const;

  // Adds without any checks (used by deserialisation; call check() after).
  void add_unchecked(Variable variable) { variables_.push_back(std::move(variable)); }

  friend bool operator==(const VariableTable&, const VariableTable&) = default;

 private:
  std::vector<Variable> variables_;
};

void hash_append(core::Hasher& h, const Variable& v);
void hash_append(core::Hasher& h, const VariableTable& t);

}  // namespace vs::feature

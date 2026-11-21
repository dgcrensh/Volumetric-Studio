#include "vs/feature/variables.hpp"

#include <algorithm>
#include <functional>
#include <queue>

#include <fmt/format.h>

namespace vs::feature {

namespace {

constexpr std::string_view kArrow = " \xE2\x86\x92 ";  // →

// Variable dependency graph: edges[i] lists the variables that variable i
// uses (by index); `unknown[i]` the names it uses that aren't variables.
struct Graph {
  std::vector<std::vector<std::size_t>> edges;
  std::vector<std::vector<std::string>> unknown;
  std::vector<std::optional<Expression>> parsed;
  std::vector<Diagnostic> parse_errors;  // per variable, code empty when fine
};

Graph build_graph(const std::vector<Variable>& vars) {
  Graph g;
  const std::size_t n = vars.size();
  g.edges.resize(n);
  g.unknown.resize(n);
  g.parsed.resize(n);
  g.parse_errors.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    auto e = Expression::parse(vars[i].expression);
    if (!e) {
      g.parse_errors[i] = e.error();
      continue;
    }
    for (const auto& ref : e->references()) {
      const auto it = std::find_if(vars.begin(), vars.end(),
                                   [&](const Variable& v) { return v.name == ref; });
      if (it == vars.end()) {
        g.unknown[i].push_back(ref);
      } else {
        g.edges[i].push_back(static_cast<std::size_t>(it - vars.begin()));
      }
    }
    g.parsed[i] = std::move(*e);
  }
  return g;
}

// Every loop the depth-first search meets, each as the variable indices
// along it (the first one is not repeated at the end).
std::vector<std::vector<std::size_t>> find_loops(const Graph& g) {
  const std::size_t n = g.edges.size();
  enum Color : unsigned char { White, Grey, Black };
  std::vector<Color> color(n, White);
  std::vector<std::size_t> stack;
  std::vector<std::vector<std::size_t>> loops;
  std::function<void(std::size_t)> visit = [&](std::size_t v) {
    color[v] = Grey;
    stack.push_back(v);
    for (const std::size_t w : g.edges[v]) {
      if (color[w] == Grey) {
        const auto from = std::find(stack.begin(), stack.end(), w);
        loops.emplace_back(from, stack.end());
      } else if (color[w] == White) {
        visit(w);
      }
    }
    stack.pop_back();
    color[v] = Black;
  };
  for (std::size_t v = 0; v < n; ++v) {
    if (color[v] == White) visit(v);
  }
  return loops;
}

// A loop through `start`, if there is one: the indices from `start` round to
// the variable that uses `start` again.
std::optional<std::vector<std::size_t>> loop_through(const Graph& g, std::size_t start) {
  std::vector<bool> seen(g.edges.size(), false);
  std::vector<std::size_t> path{start};
  std::function<bool(std::size_t)> visit = [&](std::size_t v) -> bool {
    for (const std::size_t w : g.edges[v]) {
      if (w == start) return true;
      if (seen[w]) continue;
      seen[w] = true;
      path.push_back(w);
      if (visit(w)) return true;
      path.pop_back();
    }
    return false;
  };
  if (visit(start)) return path;
  return std::nullopt;
}

Diagnostic loop_diagnostic(const VariableTable& table, const std::vector<std::size_t>& loop) {
  const auto& vars = table.variables();
  std::string path;
  std::vector<core::SubjectRef> subjects;
  for (const std::size_t i : loop) {
    path += vars[i].name;
    path += kArrow;
    subjects.push_back(table.subject(vars[i].id));
  }
  path += vars[loop.front()].name;
  return Diagnostic::error(
      "feature.variable.loop",
      fmt::format("Variables can't depend on themselves: {}. Break the loop by giving one of "
                  "them a value that doesn't use the others",
                  path),
      std::move(subjects));
}

// Problems with variable `i` on its own (name, duplicate, expression,
// unknown names), in that order.
std::vector<Diagnostic> problems_of(const VariableTable& table, const Graph& g, std::size_t i) {
  const auto& vars = table.variables();
  const Variable& v = vars[i];
  std::vector<Diagnostic> out;
  const auto subject = table.subject(v.id);
  if (!is_valid_name(v.name)) {
    out.push_back(Diagnostic::error(
        "feature.variable.bad_name",
        is_reserved_name(v.name)
            ? fmt::format("'{}' is a built-in name; choose another name for the variable",
                          v.name)
            : fmt::format("'{}' isn't a legal variable name: use a letter or '_' followed by "
                          "letters, digits or '_'",
                          v.name),
        {subject}));
  }
  for (std::size_t j = 0; j < i; ++j) {
    if (vars[j].name == v.name) {
      out.push_back(Diagnostic::error("feature.variable.duplicate_name",
                                      fmt::format("There is already a variable named '{}'", v.name),
                                      {subject, table.subject(vars[j].id)}));
      break;
    }
  }
  if (!g.parse_errors[i].code.empty()) {
    Diagnostic d = g.parse_errors[i];
    d.message = fmt::format("Variable '{}': {}", v.name, d.message);
    d.subjects.insert(d.subjects.begin(), subject);
    out.push_back(std::move(d));
  }
  for (const auto& name : g.unknown[i]) {
    out.push_back(Diagnostic::error(
        "feature.variable.unknown",
        fmt::format("Variable '{}' uses '{}', which isn't a variable", v.name, name), {subject}));
  }
  return out;
}

}  // namespace

// --- VariableValues ---------------------------------------------------------

std::optional<double> VariableValues::get(std::string_view name) const {
  const auto it = values.find(name);
  if (it == values.end()) return std::nullopt;
  return it->second;
}

Expression::Lookup VariableValues::lookup() const {
  return [this](std::string_view name) { return get(name); };
}

// --- VariableTable ----------------------------------------------------------

const Variable* VariableTable::find(const Id& id) const {
  for (const auto& v : variables_) {
    if (v.id == id) return &v;
  }
  return nullptr;
}

const Variable* VariableTable::find(std::string_view name) const {
  for (const auto& v : variables_) {
    if (v.name == name) return &v;
  }
  return nullptr;
}

core::SubjectRef VariableTable::subject(const Id& id) const {
  const Variable* v = find(id);
  return {core::SubjectKind::Variable, id, v ? v->name : std::string("(removed variable)")};
}

Result<Id> VariableTable::add(std::string name, std::string expression, Id id) {
  if (id.is_null()) id = Id::random();
  if (find(id)) {
    return Diagnostic::error("feature.variable.duplicate_id",
                             fmt::format("A variable with id {} already exists", id.to_string()));
  }
  VariableTable candidate = *this;
  candidate.variables_.push_back({id, std::move(name), std::move(expression)});
  const Graph g = build_graph(candidate.variables_);
  const std::size_t i = candidate.variables_.size() - 1;
  const auto problems = problems_of(candidate, g, i);
  if (!problems.empty()) return problems.front();
  // Nothing uses a new variable yet, so the only loop it can close is one
  // through itself ("a = a + 1").
  if (const auto loop = loop_through(g, i)) return loop_diagnostic(candidate, *loop);
  variables_ = std::move(candidate.variables_);
  return id;
}

Diagnostics VariableTable::set_expression(const Id& id, std::string expression) {
  Diagnostics out;
  const auto it = std::find_if(variables_.begin(), variables_.end(),
                               [&](const Variable& v) { return v.id == id; });
  if (it == variables_.end()) {
    out.add(Diagnostic::error("feature.variable.missing",
                              fmt::format("There is no variable with id {}", id.to_string())));
    return out;
  }
  const std::size_t i = static_cast<std::size_t>(it - variables_.begin());
  VariableTable candidate = *this;
  candidate.variables_[i].expression = std::move(expression);
  const Graph g = build_graph(candidate.variables_);
  for (auto& d : problems_of(candidate, g, i)) {
    // Name problems predate this edit; only report what the edit causes.
    if (d.code == "feature.variable.bad_name" || d.code == "feature.variable.duplicate_name") {
      continue;
    }
    out.add(std::move(d));
  }
  if (!out.empty()) return out;
  if (const auto loop = loop_through(g, i)) {
    out.add(loop_diagnostic(candidate, *loop));
    return out;
  }
  variables_ = std::move(candidate.variables_);
  return out;
}

Diagnostics VariableTable::set_expression(std::string_view name, std::string expression) {
  if (const Variable* v = find(name)) return set_expression(v->id, std::move(expression));
  Diagnostics out;
  out.add(Diagnostic::error("feature.variable.missing",
                            fmt::format("There is no variable named '{}'", name)));
  return out;
}

bool VariableTable::remove(const Id& id) {
  const auto it = std::find_if(variables_.begin(), variables_.end(),
                               [&](const Variable& v) { return v.id == id; });
  if (it == variables_.end()) return false;
  variables_.erase(it);
  return true;
}

Diagnostics VariableTable::check() const {
  Diagnostics out;
  const Graph g = build_graph(variables_);
  for (std::size_t i = 0; i < variables_.size(); ++i) {
    for (auto& d : problems_of(*this, g, i)) out.add(std::move(d));
    for (std::size_t j = 0; j < i; ++j) {
      if (variables_[j].id == variables_[i].id) {
        out.add(Diagnostic::error(
            "feature.variable.duplicate_id",
            fmt::format("Two variables share the id {}", variables_[i].id.to_string()),
            {subject(variables_[i].id)}));
      }
    }
  }
  for (const auto& loop : find_loops(g)) out.add(loop_diagnostic(*this, loop));
  return out;
}

namespace {

// Kahn's algorithm on "i uses j", always taking the earliest ready variable
// in table order, so the order is deterministic. Indices into the table;
// variables in a loop are left out. A heap of the ready ones (H5: the scan
// from the start after each pick was quadratic).
std::vector<std::size_t> order_of(const Graph& g, std::size_t n) {
  std::vector<std::size_t> pending(n, 0);
  std::vector<std::vector<std::size_t>> users(n);
  for (std::size_t i = 0; i < n; ++i) {
    for (const std::size_t j : g.edges[i]) {
      ++pending[i];
      users[j].push_back(i);
    }
  }
  std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
  for (std::size_t i = 0; i < n; ++i) {
    if (pending[i] == 0) ready.push(i);
  }
  std::vector<std::size_t> order;
  order.reserve(n);
  while (!ready.empty()) {
    const std::size_t i = ready.top();
    ready.pop();
    order.push_back(i);
    for (const std::size_t u : users[i]) {
      if (--pending[u] == 0) ready.push(u);
    }
  }
  return order;
}

}  // namespace

std::vector<Id> VariableTable::dependency_order() const {
  const Graph g = build_graph(variables_);
  std::vector<Id> out;
  for (const std::size_t i : order_of(g, variables_.size())) out.push_back(variables_[i].id);
  return out;
}

VariableValues VariableTable::evaluate() const {
  VariableValues out;
  // One graph for the order and the values (H5).
  const Graph g = build_graph(variables_);
  std::vector<bool> ordered(variables_.size(), false);
  for (const std::size_t i : order_of(g, variables_.size())) {
    ordered[i] = true;
    const Variable& v = variables_[i];
    if (!g.parse_errors[i].code.empty() || !g.unknown[i].empty()) {
      for (auto& d : problems_of(*this, g, i)) out.diagnostics.add(std::move(d));
      continue;
    }
    if (out.values.contains(v.name)) continue;  // a duplicate name; check() reports it
    auto value = g.parsed[i]->evaluate(out.lookup());
    if (!value) {
      Diagnostic d = value.error();
      if (d.code == "feature.expression.unknown_name") {
        // It uses a variable that itself failed.
        d.code = "feature.variable.upstream_failed";
        d.message = fmt::format("Variable '{}' has no value because a variable it uses has none",
                                v.name);
      } else {
        d.message = fmt::format("Variable '{}': {}", v.name, d.message);
      }
      d.subjects.insert(d.subjects.begin(), subject(v.id));
      out.diagnostics.add(std::move(d));
      continue;
    }
    out.values.emplace(v.name, *value);
  }
  bool has_loop = false;
  for (std::size_t i = 0; i < variables_.size(); ++i) has_loop |= !ordered[i];
  if (has_loop) {
    for (const auto& loop : find_loops(g)) out.diagnostics.add(loop_diagnostic(*this, loop));
  }
  return out;
}

void hash_append(core::Hasher& h, const Variable& v) {
  h.add(v.id).add(v.name).add(v.expression);
}

void hash_append(core::Hasher& h, const VariableTable& t) {
  h.add(static_cast<std::uint64_t>(t.variables().size()));
  for (const auto& v : t.variables()) h.add(v);
}

}  // namespace vs::feature

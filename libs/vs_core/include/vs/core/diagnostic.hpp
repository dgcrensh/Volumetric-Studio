#pragma once

// Diagnostics: the one error and warning type every module reports with.
//
// The spec asks that every failure point to one place the user can act on.
// A Diagnostic therefore names its subjects: the volumes, connection
// surfaces, features, nodes and so on that it is about. By convention the
// first subject is the "act here" target; the rest are context (for example,
// the second volume in an overlap, or the remaining links of a dependency
// loop, in order).
//
// Codes are stable dotted strings owned by the module that raises them, such
// as "core.settings.tolerance" or "validate.overlap". Tests match on codes,
// never on message text.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "vs/core/id.hpp"

namespace vs::core {

enum class Severity {
  Info,
  Warning,
  Error,
};

std::string_view to_string(Severity severity);

// What kind of object a diagnostic refers to.
enum class SubjectKind {
  Layout,
  Volume,
  Boundary,
  ConnectionSurface,
  PublishedOutput,
  Feature,
  Sketch,
  SketchEntity,
  Constraint,
  Variable,
  Node,
  File,
  Setting,
  KeepOut,    // a keep-out region (vs_model)
  Parameter,  // a volume's parameter (vs_model)
  Link,       // a published output feeding a variable or parameter (vs_model)
  Configuration,  // a named set of driven values (vs_model, L5)
};

std::string_view to_string(SubjectKind kind);

struct SubjectRef {
  SubjectKind kind = SubjectKind::Layout;
  Id id;
  std::string label;  // a human-readable name, for messages only

  friend bool operator==(const SubjectRef&, const SubjectRef&) = default;
};

struct Diagnostic {
  Severity severity = Severity::Error;
  std::string code;
  std::string message;
  std::vector<SubjectRef> subjects;

  static Diagnostic error(std::string code, std::string message,
                          std::vector<SubjectRef> subjects = {});
  static Diagnostic warning(std::string code, std::string message,
                            std::vector<SubjectRef> subjects = {});
  static Diagnostic info(std::string code, std::string message,
                         std::vector<SubjectRef> subjects = {});

  [[nodiscard]] bool is_error() const { return severity == Severity::Error; }

  // The subject the user should act on, if any.
  [[nodiscard]] const SubjectRef* target() const {
    return subjects.empty() ? nullptr : &subjects.front();
  }

  // One line, such as: error [core.settings.tolerance] Tolerance must be positive (Setting 'linear_tolerance_mm')
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

// An ordered collection of diagnostics.
class Diagnostics {
 public:
  void add(Diagnostic diagnostic) { items_.push_back(std::move(diagnostic)); }
  void append(const Diagnostics& other) {
    items_.insert(items_.end(), other.items_.begin(), other.items_.end());
  }

  [[nodiscard]] bool empty() const { return items_.empty(); }
  [[nodiscard]] std::size_t size() const { return items_.size(); }
  [[nodiscard]] bool has_errors() const;
  [[nodiscard]] std::size_t count(Severity severity) const;
  // Diagnostics with the given code, in order.
  [[nodiscard]] std::vector<const Diagnostic*> with_code(std::string_view code) const;

  [[nodiscard]] const std::vector<Diagnostic>& items() const { return items_; }
  [[nodiscard]] auto begin() const { return items_.begin(); }
  [[nodiscard]] auto end() const { return items_.end(); }
  const Diagnostic& operator[](std::size_t i) const { return items_[i]; }

 private:
  std::vector<Diagnostic> items_;
};

// A value or the error diagnostic that explains why there is none. Later
// modules (the kernel first) return Result<T> instead of throwing.
template <typename T>
class Result {
 public:
  Result(T value) : data_(std::move(value)) {}                 // NOLINT(google-explicit-constructor)
  Result(Diagnostic error) : data_(std::move(error)) {}        // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const { return std::holds_alternative<T>(data_); }
  explicit operator bool() const { return ok(); }

  [[nodiscard]] T& value() & { return std::get<T>(data_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(data_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(data_)); }

  [[nodiscard]] const Diagnostic& error() const { return std::get<Diagnostic>(data_); }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }

 private:
  std::variant<T, Diagnostic> data_;
};

}  // namespace vs::core

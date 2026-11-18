#include "vs/core/diagnostic.hpp"

#include <algorithm>

namespace vs::core {

std::string_view to_string(Severity severity) {
  switch (severity) {
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
  }
  return "error";
}

std::string_view to_string(SubjectKind kind) {
  switch (kind) {
    case SubjectKind::Layout: return "Layout";
    case SubjectKind::Volume: return "Volume";
    case SubjectKind::Boundary: return "Boundary";
    case SubjectKind::ConnectionSurface: return "ConnectionSurface";
    case SubjectKind::PublishedOutput: return "PublishedOutput";
    case SubjectKind::Feature: return "Feature";
    case SubjectKind::Sketch: return "Sketch";
    case SubjectKind::SketchEntity: return "SketchEntity";
    case SubjectKind::Constraint: return "Constraint";
    case SubjectKind::Variable: return "Variable";
    case SubjectKind::Node: return "Node";
    case SubjectKind::File: return "File";
    case SubjectKind::Setting: return "Setting";
  }
  return "Unknown";
}

namespace {

Diagnostic make(Severity severity, std::string code, std::string message,
                std::vector<SubjectRef> subjects) {
  Diagnostic d;
  d.severity = severity;
  d.code = std::move(code);
  d.message = std::move(message);
  d.subjects = std::move(subjects);
  return d;
}

}  // namespace

Diagnostic Diagnostic::error(std::string code, std::string message,
                             std::vector<SubjectRef> subjects) {
  return make(Severity::Error, std::move(code), std::move(message), std::move(subjects));
}

Diagnostic Diagnostic::warning(std::string code, std::string message,
                               std::vector<SubjectRef> subjects) {
  return make(Severity::Warning, std::move(code), std::move(message), std::move(subjects));
}

Diagnostic Diagnostic::info(std::string code, std::string message,
                            std::vector<SubjectRef> subjects) {
  return make(Severity::Info, std::move(code), std::move(message), std::move(subjects));
}

std::string Diagnostic::to_string() const {
  std::string out;
  out += vs::core::to_string(severity);
  out += " [";
  out += code;
  out += "] ";
  out += message;
  if (!subjects.empty()) {
    out += " (";
    for (std::size_t i = 0; i < subjects.size(); ++i) {
      if (i > 0) out += ", ";
      out += vs::core::to_string(subjects[i].kind);
      if (!subjects[i].label.empty()) {
        out += " '";
        out += subjects[i].label;
        out += "'";
      } else if (!subjects[i].id.is_null()) {
        out += " ";
        out += subjects[i].id.to_string();
      }
    }
    out += ")";
  }
  return out;
}

bool Diagnostics::has_errors() const {
  return std::any_of(items_.begin(), items_.end(),
                     [](const Diagnostic& d) { return d.is_error(); });
}

std::size_t Diagnostics::count(Severity severity) const {
  return static_cast<std::size_t>(std::count_if(
      items_.begin(), items_.end(), [&](const Diagnostic& d) { return d.severity == severity; }));
}

std::vector<const Diagnostic*> Diagnostics::with_code(std::string_view code) const {
  std::vector<const Diagnostic*> out;
  for (const auto& d : items_) {
    if (d.code == code) out.push_back(&d);
  }
  return out;
}

}  // namespace vs::core

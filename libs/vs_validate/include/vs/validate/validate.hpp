#pragma once

// Whole-document validation, as `vstudio-cli validate` and the studio's
// Validate command run it:
//
//   1. model::check(): structure, encapsulation and dependency loops
//      (model.*, feature.*). Errors here stop validation: a document that
//      breaks the authoring rules isn't built.
//   2. model::build(): the layout, boundaries, connection surfaces and the
//      contents M5 can make, of the active configuration (L5). Its
//      diagnostics are included.
//   3. validate_layout() (layout.hpp): overlap, undeclared contact and the
//      connection type rules.
//   4. check_containment() (containment.hpp) for every volume with built
//      B-rep contents.
//
// Every diagnostic comes back routed to its one place to act (registry.hpp).

#include <optional>
#include <vector>

#include "vs/core/settings.hpp"
#include "vs/model/build.hpp"
#include "vs/model/document.hpp"
#include "vs/model/project.hpp"
#include "vs/validate/layout.hpp"
#include "vs/validate/registry.hpp"

namespace vs::validate {

struct ValidationReport {
  Diagnostics diagnostics;                 // in the order above
  std::vector<RoutedDiagnostic> routed;    // the same, each with its target
  std::optional<model::DocumentBuild> build;  // none when check() failed
  LayoutReport layout;

  [[nodiscard]] bool ok() const { return !diagnostics.has_errors(); }
};

ValidationReport validate(const model::Document& document, const ProductSettings& settings,
                          const model::BlobStore* blobs = nullptr);

}  // namespace vs::validate

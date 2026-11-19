#pragma once

// The diagnostics registry: where each diagnostic sends the user.
//
// The spec asks that every failure point to one place the user can act on.
// A diagnostic's first subject is its "act here" target (vs_core), but a
// subject can be many things (a volume, a boundary, a variable, a sketch
// constraint, a link...), while the studio has only a few places to send
// the user. route() maps every diagnostic to exactly one of them:
//
//   Layout             the root layout: its features as a whole, boundaries,
//                      keep-outs, variables, volume parameters, links
//   ConnectionSurface  one connection surface's properties
//   Feature            one feature, in the layout or in one volume's
//                      parametric contents (then `volume` says which)
//   Node               one node of a volume's procedural graph (M10)
//   Contents           one volume's contents as a whole, when no feature or
//                      node is to blame (an imported file that sticks out
//                      of its boundary, say)
//
// The registry lists the codes vs_validate raises, each with its severity
// and where it routes; route() uses that entry when there is one. Codes of
// other modules (model.*, feature.*, sketch.*, kernel.*) route by their
// first subject:
//
//   Feature, Sketch, SketchEntity, Constraint  Feature (the feature that
//                                              holds it; in contents when the
//                                              document says so)
//   ConnectionSurface                          ConnectionSurface
//   Node                                       Node (the volume: the first
//                                              Volume subject after it, M10)
//   Variable                                   Layout, or Feature when a
//                                              volume's contents own it
//   Volume, with a model.contents.* code, an   Contents
//   eval.* code (the engine, M9) or a proc.*
//   code (a procedural graph, M10)
//   anything else, or no subject               Layout
//
// A diagnostic with no subject routes to the layout as a whole.

#include <optional>
#include <span>
#include <string_view>

#include "vs/core/diagnostic.hpp"
#include "vs/model/document.hpp"

namespace vs::validate {

using core::Diagnostic;
using core::Diagnostics;
using core::Id;
using core::Severity;
using core::SubjectRef;

enum class ActOn { Layout, ConnectionSurface, Feature, Node, Contents };
std::string_view to_string(ActOn where);

struct CodeInfo {
  std::string_view code;
  Severity severity = Severity::Error;
  ActOn act_on = ActOn::Layout;
  std::string_view summary;  // one line, for help text and the inspector
};

// Every code vs_validate raises, in a fixed order.
std::span<const CodeInfo> registered_codes();
// The entry for `code`, or nullptr for codes the registry doesn't list.
const CodeInfo* find_code(std::string_view code);

// The one place to act on a diagnostic.
struct ActTarget {
  ActOn on = ActOn::Layout;
  // What to select there: the diagnostic's first subject (or, for the
  // layout as a whole, an empty Layout subject).
  SubjectRef subject;
  // The volume whose contents hold the target, for Feature, Node and
  // Contents targets inside a volume; empty for the layout.
  Id volume;

  friend bool operator==(const ActTarget&, const ActTarget&) = default;
};

ActTarget route(const Diagnostic& diagnostic, const model::Document& document);

struct RoutedDiagnostic {
  Diagnostic diagnostic;
  ActTarget target;
};

std::vector<RoutedDiagnostic> route_all(const Diagnostics& diagnostics,
                                        const model::Document& document);

}  // namespace vs::validate

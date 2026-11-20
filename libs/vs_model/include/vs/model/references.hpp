#pragma once

// Reference resolution: the one place that decides what may refer to what
// (spec, design rule 1, "Encapsulation").
//
// Every reference in a document is made from a scope: the root layout, or
// the inside of one volume (its parametric contents). resolve() classifies
// the id a reference names and checks it against the scope:
//
//   from the layout, anything of the layout may be named, and of a volume
//   only its boundary (the volume's id), its connection surfaces, its
//   parameters and its published outputs. Never its contents.
//
//   from inside volume V, its own contents, its own boundary (V's id), the
//   connection surfaces that join V and V's parameters. Not the layout's
//   features, variables, keep-outs or links ("parent geometry": spec, "Edit
//   in context"), and nothing of any other volume, not even its boundary
//   (spec: "Neither volume can see the other").
//
// Errors, each naming the referencing subject first when the caller passes
// one (check.hpp does):
//
//   model.reference.contents     names something inside another volume's contents
//   model.reference.sibling      names another volume, its boundary or parameter
//   model.reference.not_input    names a connection surface that doesn't join the
//                                volume, or a published output from inside a volume
//   model.reference.parent       names the layout's geometry or variables from inside
//                                a volume
//
// Levels (L3). Inside a composite volume C is C's level: C's layout is to
// its children what the root layout is to the top-level volumes. So from
// inside C, besides C's own tree, boundary, joining surfaces and
// parameters, its children's boundaries, parameters and published outputs
// may be named, and the surfaces, keep-outs and links between them;
// anything deeper (a grandchild, a child's contents) is
// model.reference.contents, and anything of a level around C is
// model.reference.parent. From the root the same holds for the top level.
// A surface delegated down to a volume (hierarchy.hpp) is one of its
// inputs.
//
// An id that names nothing resolves to Kind::Unknown without error: whether
// that is a broken reference is for the rebuild to say, in context.
//
// There is deliberately no function that hands out a reference into a
// volume's contents: outside code holds only BoundaryRef,
// ConnectionSurfaceRef and PublishedOutputRef (document.hpp).

#include <string>
#include <string_view>

#include "vs/model/document.hpp"

namespace vs::model {

// Where a reference is made from.
struct Scope {
  Id volume;  // null: the root layout

  static Scope layout() { return {}; }
  static Scope inside(const Id& volume) { return {volume}; }
  [[nodiscard]] bool is_layout() const { return volume.is_null(); }
  friend bool operator==(const Scope&, const Scope&) = default;
};

// What an id names.
enum class TargetKind {
  Unknown,
  LayoutFeature,
  LayoutVariable,
  Volume,             // as a reference: the volume's boundary
  ConnectionSurface,
  KeepOut,
  Link,
  Parameter,          // owner: the volume
  PublishedOutput,    // owner: the volume
  InContents,         // a feature, variable, sketch or sketch entity in a
                      // volume's contents; owner: the volume
};
std::string_view to_string(TargetKind kind);

struct Target {
  TargetKind kind = TargetKind::Unknown;
  Id id;
  Id owner;  // the volume, for Parameter, PublishedOutput and InContents
  friend bool operator==(const Target&, const Target&) = default;
};

// What `id` names in the document, regardless of scope.
Target classify(const Document& document, const Id& id);

// `id` as seen from `scope`: its target, or the error that forbids the
// reference. `by` (optional) is the referencing subject, put first in the
// error's subjects.
Result<Target> resolve(const Document& document, const Scope& scope, const Id& id,
                       const SubjectRef* by = nullptr);

// The same rules for the three typed cross-boundary references, which also
// check that what they name exists ("model.reference.broken").
Result<Target> resolve(const Document& document, const Scope& scope, const BoundaryRef& ref,
                       const SubjectRef* by = nullptr);
Result<Target> resolve(const Document& document, const Scope& scope,
                       const ConnectionSurfaceRef& ref, const SubjectRef* by = nullptr);
Result<Target> resolve(const Document& document, const Scope& scope,
                       const PublishedOutputRef& ref, const SubjectRef* by = nullptr);

// A variable name as seen from `scope`: a layout variable from the layout;
// from inside a volume, a variable of its contents or one of its
// parameters. Names that resolve elsewhere give the errors above (a layout
// variable from inside a volume is model.reference.parent; a name known only
// inside some volume's contents, model.reference.contents). An unknown name
// is fine here; the rebuild reports it.
Diagnostics check_variable_name(const Document& document, const Scope& scope,
                                std::string_view name, const SubjectRef& by);

}  // namespace vs::model

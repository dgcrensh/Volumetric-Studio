#pragma once

// What a feature being authored may refer to, as labelled choices for the
// feature dialogs' pickers (plain data, no Qt).
//
// The tree being edited is the layout (volume null) or one parametric
// volume's contents ("edit in context", plan M14). A feature at position
// `index` sees only what is above it:
//
//   in the layout     the principal planes and axes; every sketch, datum and
//                     solid feature above; the faces those solid features
//                     name (rebuild.hpp), planar ones also as planes.
//
//   in a volume       only the volume's inputs (model/references.hpp): its
//                     boundary's named faces and the connection surfaces
//                     that join it (each a plane, and its face "patch");
//                     then the contents' own features above. No principal
//                     planes, nothing of the layout and nothing of any other
//                     volume, so a choice can never break encapsulation.
//
// Faces are listed from the latest evaluation, so a feature that failed
// (or a boundary that didn't build) offers no faces until it builds.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "vs/core/id.hpp"
#include "vs/eval/engine.hpp"
#include "vs/feature/tree.hpp"

namespace vs::app {

using core::Id;

template <typename Ref>
struct Choice {
  std::string label;
  Ref ref;
};

struct ReferenceChoices {
  std::vector<Choice<feature::PlaneRef>> planes;
  std::vector<Choice<feature::AxisRef>> axes;
  std::vector<Choice<feature::PointRef>> points;
  std::vector<Choice<feature::FaceRef>> faces;      // planar and cylindrical
  std::vector<Choice<Id>> sketches;                 // sketch features
  std::vector<Choice<Id>> solid_features;           // extrude, revolve, sweep, loft (patternable)
  std::vector<std::string> variables;               // names expressions may use

  // The label of a reference, or empty when it isn't among the choices.
  [[nodiscard]] std::string label_of(const feature::PlaneRef& ref) const;
  [[nodiscard]] std::string label_of(const feature::AxisRef& ref) const;
  [[nodiscard]] std::string label_of(const feature::PointRef& ref) const;
  [[nodiscard]] std::string label_of(const feature::FaceRef& ref) const;
};

// The tree `volume` edits (null: the layout); nullptr when the volume has
// no parametric contents.
const feature::FeatureTree* tree_of(const model::Document& document, const Id& volume);
feature::FeatureTree* tree_of(model::Document& document, const Id& volume);

// The rebuild result of that tree in `evaluation`, if it has one.
const feature::RebuildResult* rebuild_of(const eval::Evaluation& evaluation, const Id& volume);

// Choices for a feature at `index` of the tree (nullopt: after the last
// feature, as for a new one).
ReferenceChoices reference_choices(const eval::Evaluation& evaluation, const Id& volume,
                                   std::optional<std::size_t> index = std::nullopt);

// Choices for connection surfaces, which the layout owns: the layout's
// sketches (sketch patches) and planes (datum patches).
ReferenceChoices layout_choices(const eval::Evaluation& evaluation);

}  // namespace vs::app

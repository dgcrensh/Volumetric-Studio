#pragma once

// Dimension dragging (M16): the numbers that size a feature, as handles the
// user drags in the viewport (or, in sketch mode, as dimension labels).
// Plain data and rules, no Qt.
//
// A selected sketch offers its dimensions; a selected extrude its depth and
// its sketch's dimensions. Each handle sits on the geometry it sizes (a
// line's middle, a circle's rim, the extrude's far end) and moves along the
// direction that makes the value grow. Dragging it sets what drives the
// number:
//
//   - a dimension or depth driven by one variable ("width") drags the
//     variable, so everything else that uses it follows, as the design says;
//   - a plain number drags the number (sketch dimensions are solved again,
//     M2's drag solving keeping the rest of the sketch in place);
//   - one driven by a longer expression ("width / 2") has no handle: there
//     is no one right number to change.
//
// While the handle moves the app previews the edit without generating
// (boundaries, and cached contents of untouched volumes, M13's preview
// tier); releasing it commits one undoable step and generates.

#include <optional>
#include <string>
#include <vector>

#include "history.hpp"
#include "vs/core/settings.hpp"
#include "vs/core/vec.hpp"
#include "vs/eval/engine.hpp"
#include "vs/sketch/sketch.hpp"

namespace vs::app {

using core::Id;

struct DimensionHandle {
  enum class Target { Variable, SketchDimension, FeatureParam };
  Target target = Target::SketchDimension;
  Id tree;        // the volume whose tree holds the feature; null: the layout
  Id feature;     // the sketch feature (dimensions) or the extrude (depth)
  Id constraint;  // SketchDimension, and Variable when a dimension names it
  Id variable;    // Variable: the variable that drives the number
  std::string label;   // "Base outline: Distance 1 (width)"
  double value = 0.0;  // now, mm
  double rate = 1.0;   // value change per mm the handle moves
  core::dvec3 anchor{};     // where the handle is drawn, model space
  core::dvec3 direction{};  // unit: moving this way makes the value grow
};

// A sketch dimension's handle in the sketch's own plane: where its label
// would anchor, the way that grows the value, and the rate. Nullopt for
// dimensions that can't be dragged along a line (angles).
struct SketchHandle {
  core::dvec2 anchor{};
  core::dvec2 direction{};
  double rate = 1.0;
};
std::optional<SketchHandle> sketch_dimension_handle(const sketch::Sketch& sketch, const sketch::Constraint& constraint);

// The handles of `feature` in `tree`'s tree (null: the layout), placed from
// the evaluation's rebuild. Empty for features with nothing to drag.
std::vector<DimensionHandle> dimension_handles(const eval::Evaluation& evaluation, const Id& tree, const Id& feature);

// The value a handle reaches after moving `distance` mm along its
// direction from where it started (never below `minimum`).
double dragged_value(const DimensionHandle& handle, double distance, double minimum = 0.01);

// The edit that sets the handle's number to `value`: one Command, labelled
// "Drag <label>".
Command drag_command(const DimensionHandle& handle, double value, core::ProductSettings settings);

}  // namespace vs::app

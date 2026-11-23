#pragma once

// Sketch mode's editing model (plan, M14: "draw lines, arcs, circles,
// rectangles and slots on a plane; add constraints and dimensions; live
// solving with DOF shown; conflicts highlighted by name"). Plain data and
// rules, no Qt: sketch_view.* draws it and turns mouse clicks into calls.
//
// A SketchEditor edits a copy of one sketch. Every change (a finished
// shape, a constraint, a dimension, a deletion, a drag) is followed by a
// solve, whose report says the status, the degrees of freedom left, what
// can still move and, when constraints conflict, which ones by name. A
// conflicting constraint is kept, so the user sees it (drawn red) and
// decides what to remove; the solver leaves the geometry where it was.
//
// Drawing tools take clicks in sketch coordinates. A click may snap to an
// existing point: the new curve then shares that point, which joins the
// two curves without a coincident constraint (sketch.hpp).
//
//   Point       one click
//   Line        a click per vertex; each line starts where the last ended
//               (a chain) until finish_tool(). Lines within 2 degrees of
//               the sketch axes get a horizontal or vertical constraint.
//   Rectangle   two opposite corners: four lines, two horizontal and two
//               vertical constraints
//   Circle      centre, then a point on the circle
//   Arc         centre, start, end (counter-clockwise; the end is moved
//               onto the start's radius)
//   Slot        the two cap centres, then a point giving the radius: two
//               lines tangent to two arcs of equal radius
//   Curve       a cubic Bézier: start, first handle, second handle, end
//               (the control polygon in order; S1). Chained like lines: the
//               next curve starts at the last one's end until finish_tool().
//               Only the ends snap; handles are new points.
//
// import_svg() adds an SVG file's outlines (vs_sketch's import_svg()) as one
// step, like a finished shape.
//
// The editor keeps its own undo stack for the session; finishing the
// sketch hands the result to the app's History as one step.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vs/core/settings.hpp"
#include "vs/feature/expression.hpp"
#include "vs/sketch/sketch.hpp"
#include "vs/sketch/solver.hpp"
#include "vs/sketch/svg.hpp"

namespace vs::app {

// Dimensions whose value has a sign: a horizontal or vertical distance
// (second minus first) and an angle. Dragging one may cross zero; dragging
// any other stops at kMinDraggedDimension (H5).
bool signed_dimension(sketch::ConstraintKind kind);
inline constexpr double kMinDraggedDimension = 0.01;  // mm

using core::dvec2;
using core::Id;

enum class SketchTool { Select, Point, Line, Rectangle, Circle, Arc, Slot, Bezier };
std::string_view to_string(SketchTool tool);

class SketchEditor {
 public:
  // `variables` looks names up for dimension expressions (layout variables,
  // or a volume's parameters); may be empty.
  SketchEditor(sketch::Sketch sketch, core::ProductSettings settings,
               feature::Expression::Lookup variables = {});

  [[nodiscard]] const sketch::Sketch& sketch() const { return sketch_; }
  [[nodiscard]] const sketch::SolveReport& report() const { return report_; }
  [[nodiscard]] bool changed() const { return changed_; }

  // --- solving
  // Solves again (after the variables changed, say).
  const sketch::SolveReport& solve();
  [[nodiscard]] int dof() const { return report_.dof; }
  [[nodiscard]] sketch::SolveStatus status() const { return report_.status; }
  // "Distance 2", ... for the conflicting (and redundant) constraints.
  [[nodiscard]] std::vector<std::string> conflict_labels() const;
  [[nodiscard]] std::vector<std::string> redundant_labels() const;
  [[nodiscard]] bool is_conflicting(const Id& constraint) const;
  [[nodiscard]] bool is_free(const Id& entity) const;  // can still move (lines: an end can)
  // One line for the status bar: "Under-constrained: 3 degrees of freedom".
  [[nodiscard]] std::string status_text() const;

  // --- drawing
  void set_tool(SketchTool tool);
  [[nodiscard]] SketchTool tool() const { return tool_; }
  // A click at `p`; `snap` is an existing point to reuse instead (null:
  // none). Returns the entities a completed shape added (empty while the
  // shape still needs clicks).
  std::vector<Id> click(dvec2 p, const Id& snap = {});
  // The clicks of the shape in progress, for drawing a rubber band.
  [[nodiscard]] const std::vector<dvec2>& pending() const { return pending_; }
  // Where the next line of a chain starts (null: no chain).
  [[nodiscard]] const Id& chain_end() const { return chain_end_; }
  // Ends a chain of lines (or drops an unfinished shape).
  void finish_tool();

  // The nearest point entity within `radius`, or null.
  [[nodiscard]] Id nearest_point(dvec2 p, double radius) const;
  // The nearest point or curve within `radius` (points win ties), or null.
  [[nodiscard]] Id nearest_entity(dvec2 p, double radius) const;
  // Distance from `p` to an entity's geometry; infinity for unknown ids.
  [[nodiscard]] double distance_to(const Id& entity, dvec2 p) const;

  // --- importing (S1)
  // Adds an SVG document's outlines as one undo step and solves. On error
  // nothing changes. An unfinished shape is dropped first.
  core::Result<sketch::SvgImport> import_svg(std::string_view svg, const sketch::SvgImportOptions& options);

  // --- constraints and dimensions
  // Whether `kind` fits `refs` (sketch.hpp lists the forms).
  [[nodiscard]] bool fits(sketch::ConstraintKind kind, const std::vector<Id>& refs) const;
  // The constraint kinds that fit a selection, in a fixed order.
  [[nodiscard]] std::vector<sketch::ConstraintKind> applicable(const std::vector<Id>& refs) const;
  // The current value a dimension of `kind` on `refs` would have.
  [[nodiscard]] std::optional<double> measure(sketch::ConstraintKind kind, const std::vector<Id>& refs) const;
  // Adds a constraint; a dimensional one takes `value`, or its measured
  // value, and an optional expression. Null when it doesn't fit.
  Id constrain(sketch::ConstraintKind kind, std::vector<Id> refs, std::optional<double> value = std::nullopt,
               std::string expression = {});
  // Changes a dimension: its value, and its expression (empty: none; set:
  // evaluated with the variables, which must know its names).
  core::Diagnostics set_dimension(const Id& constraint, double value, std::string expression = {});

  // --- other edits
  // Removes entities (with curves that use a removed point, and points
  // only removed curves used) and constraints, and every constraint that
  // referred to anything removed.
  bool remove(const std::vector<Id>& ids);
  bool toggle_construction(const std::vector<Id>& ids);
  bool rename(const Id& id, std::string name);
  // Drags a point towards `target` (drag solving). `record` false merges it
  // into the last step, so one mouse drag is one undo step.
  bool drag(const Id& point, dvec2 target, bool record = true);
  // Drags a dimension to `value` (M16: dimension dragging in the
  // sketcher): solved from where the sketch is, so the rest moves as little
  // as it can. A dimension driven by an expression is refused
  // (app.sketch.dimension_driven): its variables decide it. `record` as
  // for drag().
  // A length, radius or diameter stays at least kMinDraggedDimension; a
  // signed dimension (signed_dimension()) takes any value, through zero (H5).
  core::Diagnostics drag_dimension(const Id& constraint, double value, bool record = true);

  [[nodiscard]] bool can_undo() const { return !undo_.empty(); }
  [[nodiscard]] bool can_redo() const { return !redo_.empty(); }
  bool undo();
  bool redo();
  // Undoes the last step and forgets it (no redo): a preview being
  // replaced or cancelled.
  bool undo_and_forget();

 private:
  void record();
  Id point_at(dvec2 p, const Id& snap);
  void evaluate_dimensions();
  std::vector<Id> finish_shape();

  sketch::Sketch sketch_;
  core::ProductSettings settings_;
  feature::Expression::Lookup variables_;
  std::unique_ptr<sketch::SketchSolver> solver_;
  sketch::SolveReport report_;
  SketchTool tool_ = SketchTool::Select;
  std::vector<dvec2> pending_;
  std::vector<Id> pending_snaps_;
  Id chain_end_;  // the last point of a chain of lines
  bool changed_ = false;
  std::vector<sketch::Sketch> undo_;
  std::vector<sketch::Sketch> redo_;
};

}  // namespace vs::app

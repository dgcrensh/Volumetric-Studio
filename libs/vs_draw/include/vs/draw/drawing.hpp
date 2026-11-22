#pragma once

// Drawings from evaluations (L6, decision 0038).
//
// draw() lays out one drawing of the evaluated document (its definition is
// in model::Document::drawings) on a Sheet:
//
//   - each view projects what it shows (view.hpp): the shown volumes'
//     boundaries as phantom lines, the connection surfaces joining them as
//     dash-dot outlines, their contents with hidden lines removed (and
//     hidden edges dashed when the view asks), centred on the sheet at the
//     view's position by the centre of the shown boundaries, which only the
//     layout moves; a label with the view's name and scale goes under it;
//   - each dimension is measured from its anchors and drawn: extension
//     lines, a dimension line with arrows and the value (decimals per the
//     drawing, trailing zeros dropped), or a diameter's leader. Disposable
//     ones (on contents) are in parentheses and in their own pen;
//   - a border and a title block: the drawing's and document's names, the
//     configuration, each view's scale, the units, and what parentheses
//     mean.
//
// A drawing is computed from the evaluation every time: nothing measured is
// stored, so a disposable dimension follows its contents through every
// regeneration, and nothing on a sheet can drive the model.
//
// Measuring. A linear dimension measures |m · (b - a)| where m is the
// view's right (horizontal) or up (vertical) axis, from each anchor's
// position along m:
//
//   origin                 0
//   boundary min / max     the boundary's exact extreme along m (OCCT's
//                          optimal box)
//   boundary centre        the centre of its box
//   boundary face          the named face's plane, which must face along m,
//                          or a cylinder's axis, which must face the viewer
//   surface min / max      the patch's exact extreme
//   surface centre         a planar patch's box centre; a cylindrical
//                          patch's axis, which must face the viewer
//   surface hole k         the k-th bolt hole's centre
//   contents min / max     the contents' extreme: exact for B-rep, the
//                          vertices' for meshes; a composite's are its
//                          leaves', generated children included
//   contents centre        the centre of the contents' box
//   contents face          a named face of parametric contents
//                          ("<feature id>:<name>"), as a boundary face
//
// A diameter is of a cylindrical boundary or contents face, a cylindrical
// patch or a bolt hole (its thread's ISO 273 clearance hole), facing the
// viewer.
//
// Codes:
//   draw.drawing.missing         no drawing of that id or name (error)
//   draw.view.empty              a view shows nothing (warning)
//   draw.view.failed             part of a view couldn't be projected (warning)
//   draw.contents.stale          a shown volume's contents aren't current:
//                                its last valid contents are drawn (warning)
//   draw.dimension.unresolved    a stable anchor isn't there: its boundary or
//                                surface failed, no face or hole of that
//                                name (error)
//   draw.dimension.not_parallel  a face or axis that doesn't fit the
//                                dimension's direction (error)
//   draw.dimension.not_circle    a diameter of something that isn't a circle
//                                facing the viewer (error)
//   draw.dimension.lost          a disposable dimension whose anchor is gone
//                                or no longer fits: not drawn (warning, in
//                                place of the three above)
// The first subject is the drawing item (SubjectKind::Drawing), then the
// volume or surface concerned.

#include <optional>
#include <string>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/core/settings.hpp"
#include "vs/draw/sheet.hpp"
#include "vs/draw/view.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/build.hpp"
#include "vs/model/document.hpp"

namespace vs::draw {

struct DrawOptions {
  // In sheet mm; divided by each view's scale for the model.
  double deflection = 0.02;
  double step = 0.1;
  double simplify = 0.02;
  double text_height = 3.5;
  core::ProductSettings settings;  // the engine's (tessellation needs them)
  validate::ParallelFor parallel;  // empty: the calling thread
};

struct DimensionResult {
  Id dimension;
  bool disposable = false;
  bool drawn = false;
  std::optional<double> value;  // mm; none when it couldn't be measured
  std::string text;             // as written on the sheet
};

struct ViewResult {
  Id view;
  std::size_t visible_lines = 0;
  std::size_t hidden_lines = 0;
  std::size_t outline_lines = 0;
};

struct DrawingResult {
  Sheet sheet;
  std::vector<ViewResult> views;            // in drawing order
  std::vector<DimensionResult> dimensions;  // in drawing order
  core::Diagnostics diagnostics;
  [[nodiscard]] bool ok() const { return !diagnostics.has_errors(); }
  [[nodiscard]] const DimensionResult* find(const Id& dimension) const;
};

// Draws `drawing`, which needn't be in the evaluated document (a standard
// drawing not yet saved, say). Anchors and shown volumes are looked up in
// `evaluation.document`.
DrawingResult draw(const eval::Evaluation& evaluation, const model::Drawing& drawing,
                   const DrawOptions& options = {});

// Draws the evaluated document's drawing of that id or name.
DrawingResult draw(const eval::Evaluation& evaluation, const Id& drawing,
                   const DrawOptions& options = {});
DrawingResult draw(const eval::Evaluation& evaluation, std::string_view drawing,
                   const DrawOptions& options = {});

// The largest standard scale (50:1 ... 1:1000, in 1-2-5 steps) not above
// `fit`; 1:1000 below that.
double standard_scale(double fit);
// "1:2", "5:1", "1:1".
std::string scale_text(double scale);
// A number as a drawing writes it: at most `decimals`, trailing zeros
// dropped, never "-0".
std::string number_text(double value, int decimals);

// A drawing to start from: front, top, right and isometric views of every
// volume, third-angle (top above front, right to its right, isometric in
// the corner) at the largest standard scale that fits the sheet above the
// title block, with the layout's overall width and height (front view) and
// depth (top view) as stable dimensions between the boundaries at its
// extremes. `layout` is the document's layout build (model::build_layout()
// or an evaluation's `build`).
model::Drawing standard_drawing(const model::Document& document,
                                const model::DocumentBuild& layout, std::string name,
                                double width = 420.0, double height = 297.0);

}  // namespace vs::draw

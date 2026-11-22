#pragma once

// Drawings in the app (L6, decision 0038): the commands that edit them and
// what the Drawings dock and the dimension dialog list. Plain data, no Qt.
//
// Every change is one undoable Command (history.hpp). History drops views'
// volumes and dimensions whose anchors an edit removed
// (model::prune_drawings()), so deleting a volume or a surface is never
// refused because a drawing showed or dimensioned it.

#include <optional>
#include <string>
#include <vector>

#include "history.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/document.hpp"

namespace vs::app {

using core::Id;

// --- commands ---------------------------------------------------------------------------------

// Adds `drawing` (vs_draw::standard_drawing() or model::make_drawing()),
// its name made unique among the drawings; a null id becomes a random one.
Command add_drawing(model::Drawing drawing);
Command rename_drawing(Id drawing, std::string name);
Command remove_drawing(Id drawing);
// The sheet's size (mm) and how many decimals dimensions show.
Command set_sheet(Id drawing, double width, double height, int decimals);

// Adds a view (a null id becomes a random one; its name made unique on
// the drawing), or replaces the view of the same id.
Command add_view(Id drawing, model::DrawingView view);
Command update_view(Id drawing, model::DrawingView view);
// Removes a view and its dimensions.
Command remove_view(Id drawing, Id view);

// Adds a dimension (a null id becomes a random one) or replaces the one of
// the same id.
Command add_dimension(Id drawing, model::DrawingDimension dimension);
Command update_dimension(Id drawing, model::DrawingDimension dimension);
Command remove_dimension(Id drawing, Id dimension);
// Moves a dimension's line: its offset from the anchors, sheet mm.
Command move_dimension(Id drawing, Id dimension, double offset);

// --- what the dock lists ----------------------------------------------------------------------

enum class DrawingRowKind { Drawing, View, Dimension };

struct DrawingRow {
  DrawingRowKind kind = DrawingRowKind::Drawing;
  Id id;
  Id drawing;  // its drawing (itself for a drawing)
  int depth = 0;
  std::string label;  // "Sheet 1", "Front (front, 1:2)", "Horizontal: Base boundary min to ..."
  bool disposable = false;  // a dimension on contents
};

// Drawings, each followed by its views, each followed by its dimensions.
std::vector<DrawingRow> drawing_rows(const model::Document& document);

// --- what the dimension dialog offers ---------------------------------------------------------

struct AnchorChoice {
  model::Anchor anchor;
  std::string label;  // disposable ones in parentheses, as on the sheet
  bool disposable = false;
};

// The anchors `view` offers a dimension of `kind`, from what `evaluation`
// shows: for a horizontal or vertical one the origin, then for each shown
// volume its boundary's min, max, centre and the planar faces facing along
// the view's sheet axes, each joining connection surface's min, max,
// centre and bolt holes, and each shown volume's contents' min, max,
// centre and parametric faces facing along the axes; for a diameter only
// circles facing the viewer (cylindrical boundary and contents faces,
// cylindrical patches, bolt holes). Stable choices come first.
std::vector<AnchorChoice> anchor_choices(const eval::Evaluation& evaluation, const model::DrawingView& view,
                                         model::DimensionKind kind);

// What an anchor reads as in the dock and the dialog.
std::string anchor_label(const model::Document& document, const model::Anchor& anchor);

}  // namespace vs::app

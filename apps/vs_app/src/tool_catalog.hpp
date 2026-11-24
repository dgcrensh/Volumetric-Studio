#pragma once

// The context toolbar's rules (U1, decision 0039). Plain data, no Qt.
//
// The window works in one context at a time, and a row of tabs above one
// toolbar shows which; the toolbar holds that context's tools:
//
//   Layout    volumes, connection surfaces, keep-outs, links, layout
//             geometry, and what fills the selected volume
//   Contents  the features of the volume whose contents are edited in context
//   Sketch    drawing, constraining and dimensioning the open sketch
//   Graph     the procedure being edited in the node editor
//   Drawing   the drawing on the sheet
//
// The context is never stored: current_context() derives it from what the
// window has open, so the tabs can't disagree with the window. Every tool
// is one row of the catalogue (id, label, group, tooltip, shortcut, icon);
// the window makes one QAction per row and shares it between the toolbar,
// the menus and the right-click menus. availability() says whether a tool
// can act on what is selected now, and why not when it can't, so the
// toolbar greys it out with a reason instead of failing on a click.

#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "icons.hpp"
#include "outline.hpp"
#include "vs/core/id.hpp"
#include "vs/feature/tree.hpp"
#include "vs/model/document.hpp"
#include "vs/sketch/sketch.hpp"

namespace vs::app {

enum class Context { Layout, Contents, Sketch, Graph, Drawing };
inline constexpr Context kContexts[] = {Context::Layout, Context::Contents, Context::Sketch, Context::Graph,
                                        Context::Drawing};
std::string_view to_string(Context context);  // "layout", ...
std::string_view title(Context context);      // the tab's label: "Layout", ...

enum class ToolId {
  // Layout: create
  NewVolume,
  NewConnection,
  NewKeepOut,
  NewLink,
  // Layout and Contents: geometry of the tree being edited
  Sketch,
  DatumPlane,
  DatumAxis,
  DatumPoint,
  Extrude,
  Revolve,
  Sweep,
  Loft,
  Shell,
  Fillet,
  Chamfer,
  LinearPattern,
  CircularPattern,
  // Layout: fill the selected volume
  EditContents,
  EditGraph,
  AttachTemplate,
  ImportContents,
  // Layout: structure
  MakeComposite,
  GenerateChildren,
  // Layout: check
  Validate,
  // Contents
  LeaveContext,
  // Sketch: draw
  SketchSelect,
  SketchPoint,
  SketchLine,
  SketchRectangle,
  SketchCircle,
  SketchArc,
  SketchSlot,
  SketchBezier,
  // Sketch: insert
  ImportSvg,
  // Sketch: modify (decision 0041)
  OffsetCurves,
  // Sketch: constrain and dimension (one per sketch::ConstraintKind)
  Coincident,
  PointOnCurve,
  Horizontal,
  Vertical,
  Parallel,
  Perpendicular,
  Tangent,
  Equal,
  Symmetric,
  Fix,
  Distance,
  HorizontalDistance,
  VerticalDistance,
  Angle,
  Radius,
  Diameter,
  // Sketch: edit and finish
  Construction,
  DeleteSketchItems,
  CancelSketch,
  FinishSketch,
  // Graph
  Bake,
  Unbake,
  LeaveGraph,
  // Drawing
  NewDrawing,
  AddDrawingView,
  AddDrawingDimension,
  ExportDrawing,
  CloseDrawing,
};

struct Tool {
  ToolId id;
  std::string_view name;      // stable, for tests and saved settings: "new_volume"
  std::string_view label;     // under the icon: "Volume"
  std::string_view tooltip;   // one sentence
  std::string_view shortcut;  // portable text ("Ctrl+Shift+E"); empty: none
  Icon icon;
};

// Every tool, in ToolId order.
std::span<const Tool> all_tools();
const Tool& tool(ToolId id);
std::optional<ToolId> find_tool(std::string_view name);

// A context's toolbar: named groups of tools, left to right.
struct ToolGroup {
  std::string_view name;  // the caption under the group: "Create"
  std::vector<ToolId> tools;
  // How many of `tools` are buttons; the rest are in the group's "More"
  // menu (everything when it is at least tools.size()).
  std::size_t shown = std::numeric_limits<std::size_t>::max();
};
const std::vector<ToolGroup>& tool_groups(Context context);
// Whether a tool is on a context's toolbar.
bool shows_in(ToolId id, Context context);
// The feature a geometry tool inserts (nullopt for other tools), and the
// constraint a constrain or dimension tool adds.
std::optional<feature::FeatureKind> feature_kind(ToolId id);
std::optional<sketch::ConstraintKind> constraint_kind(ToolId id);

// What the window has open and selected; the main window fills it in.
struct ToolState {
  // Sketch mode is open (on any tree).
  bool sketch_open = false;
  // The sketch's selected entities and constraints, and the constraint
  // kinds that fit them (SketchEditor::applicable()).
  std::size_t sketch_selection = 0;
  // How many of them are curves (lines, arcs, circles, Béziers).
  std::size_t sketch_curve_selection = 0;
  std::vector<sketch::ConstraintKind> applicable_constraints;
  // A drawing is shown on the sheet.
  bool drawing_shown = false;
  // Whether there is an evaluation to draw from.
  bool evaluated = false;
  // The volume whose graph the node editor shows (null: none).
  core::Id graph_volume;
  // The volume whose contents (a parametric tree) or layout (a composite)
  // are edited in context (null: the root layout).
  core::Id editing_volume;
  Selection selection;
  // The shown evaluation made the selected volume (L4): read-only.
  bool selection_generated = false;
};

// The context the window is in: sketch mode, then a shown drawing, then an
// open graph, then contents edited in context; otherwise the layout (a
// composite edited in context is a layout too: its own).
Context current_context(const model::Document& document, const ToolState& state);

// Whether something can be used now, and why not when it can't.
struct Availability {
  bool enabled = true;
  std::string reason;  // empty when enabled: one sentence for the tooltip
  explicit operator bool() const { return enabled; }
};

// The tabs: whether clicking each would enter it from here. The current
// context is always enabled. While a sketch is open every other tab waits
// for Finish or Cancel.
Availability tab_availability(Context tab, const model::Document& document, const ToolState& state);

// Whether a tool can act now. Tools of other contexts than the current one
// are never enabled.
Availability availability(ToolId id, const model::Document& document, const ToolState& state);

// The volume a selection names directly (a volume selection; null else).
core::Id selected_volume(const ToolState& state);
// Whether the selection is a sketch feature (in the layout or a volume).
bool selection_is_sketch(const model::Document& document, const Selection& selection);

}  // namespace vs::app

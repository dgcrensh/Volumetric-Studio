#pragma once

// The app's icons (U1, decision 0039) as plain stroke geometry, no Qt.
//
// Every icon is drawn on a 24 x 24 grid, y down, as a few strokes: open or
// closed polylines (circles and arcs already flattened) and dots. The UI
// paints them with a round 1.75-unit pen in the text colour at any size,
// so they stay crisp at any scale and follow the theme, and no image files
// or extra Qt module are needed. The shapes match the U1 mockups.

#include <span>
#include <string_view>
#include <vector>

namespace vs::app {

enum class Icon {
  // layout and contents
  Volume,
  Surface,
  KeepOut,
  Link,
  Sketch,
  Plane,
  Axis,
  Point,
  Extrude,
  Revolve,
  Sweep,
  Loft,
  Shell,
  Fillet,
  Chamfer,
  LinearPattern,
  CircularPattern,
  Model,
  Procedure,
  Template,
  Import,
  Composite,
  Generate,
  Validate,
  Done,
  // sketch
  Select,
  Line,
  Rectangle,
  Circle,
  Arc,
  Slot,
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
  Construction,
  Delete,
  Cancel,
  // procedures and drawings
  Bake,
  Unbake,
  Drawing,
  View,
  Export,
  Close,
  // the window
  Regenerate,
  Undo,
  Redo,
  FitAll,
  Error,
  Warning,
  ChevronLeft,
  ChevronRight,
  ChevronUp,
  ChevronDown,
  Browser,
  Variables,
  // S1
  Bezier,
  ImportSvg,
  // Decision 0041
  Offset,
};

inline constexpr double kIconGrid = 24.0;

struct IconPoint {
  double x = 0;
  double y = 0;
  friend bool operator==(const IconPoint&, const IconPoint&) = default;
};

struct IconStroke {
  std::vector<IconPoint> points;
  bool closed = false;  // joins the last point back to the first
  bool dot = false;     // one point, drawn as a filled dot
  bool dashed = false;  // drawn dashed (construction geometry)
};

// Every icon, in enum order.
std::span<const Icon> all_icons();
std::string_view to_string(Icon icon);  // "volume", ...
// The icon's strokes, inside [0, kIconGrid] on both axes; never empty.
const std::vector<IconStroke>& icon_strokes(Icon icon);

}  // namespace vs::app

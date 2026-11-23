#include "icons.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <numbers>

namespace vs::app {

namespace {

using Strokes = std::vector<IconStroke>;

IconStroke poly(std::initializer_list<IconPoint> points, bool closed = false) {
  return {std::vector<IconPoint>(points), closed, false, false};
}

IconStroke line(double x0, double y0, double x1, double y1) { return poly({{x0, y0}, {x1, y1}}); }

IconStroke rect(double x, double y, double w, double h) {
  return poly({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, true);
}

IconStroke dot(double x, double y) { return {{{x, y}}, false, true, false}; }

IconStroke dashed(IconStroke s) {
  s.dashed = true;
  return s;
}

// Points along an arc, angles in degrees on the y-down grid (0 = +x,
// 90 = down), from a0 to a1 inclusive.
std::vector<IconPoint> arc_points(double cx, double cy, double r, double a0, double a1) {
  const int steps = std::max(4, static_cast<int>(std::ceil(std::abs(a1 - a0) / 12.0)));
  std::vector<IconPoint> out;
  out.reserve(static_cast<std::size_t>(steps) + 1);
  for (int i = 0; i <= steps; ++i) {
    const double a = (a0 + (a1 - a0) * i / steps) * std::numbers::pi / 180.0;
    out.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
  }
  return out;
}

IconStroke arc(double cx, double cy, double r, double a0, double a1) {
  return {arc_points(cx, cy, r, a0, a1), false, false, false};
}

IconStroke circle(double cx, double cy, double r) {
  IconStroke s{arc_points(cx, cy, r, 0, 360), true, false, false};
  s.points.pop_back();  // closed: the last point is the first
  return s;
}

// A stadium round the segment (x0, y0) - (x1, y1).
IconStroke capsule(double x0, double y0, double x1, double y1, double r) {
  const double a = std::atan2(y1 - y0, x1 - x0) * 180.0 / std::numbers::pi;
  IconStroke s{arc_points(x1, y1, r, a - 90, a + 90), true, false, false};
  const auto back = arc_points(x0, y0, r, a + 90, a + 270);
  s.points.insert(s.points.end(), back.begin(), back.end());
  return s;
}

// Joins pieces into one stroke.
IconStroke join(std::initializer_list<std::vector<IconPoint>> pieces) {
  IconStroke s;
  for (const auto& p : pieces) s.points.insert(s.points.end(), p.begin(), p.end());
  return s;
}

IconStroke mirrored(IconStroke s) {
  for (auto& p : s.points) p.x = kIconGrid - p.x;
  return s;
}

Strokes make(Icon icon) {
  switch (icon) {
    case Icon::Volume:
      return {poly({{12, 3}, {20, 7.5}, {20, 16.5}, {12, 21}, {4, 16.5}, {4, 7.5}}, true), line(12, 12, 20, 7.5),
              line(12, 12, 12, 21), line(12, 12, 4, 7.5)};
    case Icon::Surface: return {poly({{4, 9}, {12, 5}, {20, 9}, {12, 13}}, true), poly({{4, 15}, {12, 19}, {20, 15}})};
    case Icon::KeepOut: return {circle(12, 12, 9), line(5.6, 5.6, 18.4, 18.4)};
    case Icon::Link: return {capsule(6, 18, 11, 13, 3), capsule(13, 11, 18, 6, 3), line(10, 14, 14, 10)};
    case Icon::Sketch: return {poly({{4, 20}, {8, 19}, {19, 8}, {16, 5}, {5, 16}}, true), line(14, 7, 17, 10)};
    case Icon::Plane: return {poly({{3, 16}, {9, 8}, {21, 8}, {15, 16}}, true)};
    case Icon::Axis: return {dashed(line(12, 3, 12, 21)), poly({{9, 6}, {12, 3}, {15, 6}})};
    case Icon::Point: return {dot(12, 12), line(12, 5, 12, 8), line(12, 16, 12, 19), line(5, 12, 8, 12), line(16, 12, 19, 12)};
    case Icon::Extrude:
      return {poly({{4, 18}, {10, 14}, {20, 14}, {14, 18}}, true), line(12, 11, 12, 3), poly({{9, 6}, {12, 3}, {15, 6}})};
    case Icon::Revolve:
      return {dashed(line(6, 3, 6, 21)), poly({{6, 6}, {11, 6}, {13, 12}, {11, 18}, {6, 18}}), arc(6, 12, 12, -30, 30),
              poly({{15.5, 15.5}, {16.4, 18}, {18.8, 17}})};
    case Icon::Sweep: return {circle(6, 18, 3), poly({{8.5, 16}, {12, 11}, {20, 7}})};
    case Icon::Loft: {
      IconStroke top = circle(12, 6, 5);
      for (auto& p : top.points) p.y = 6 + (p.y - 6) * 0.5;
      return {poly({{4, 18}, {12, 21}, {20, 18}, {12, 15}}, true), top, line(4, 18, 7, 6), line(20, 18, 17, 6)};
    }
    case Icon::Shell: return {rect(4, 4, 16, 16), poly({{8, 4}, {8, 16}, {16, 16}, {16, 4}})};
    case Icon::Fillet: return {join({{{4, 20}}, arc_points(11, 11, 7, 180, 270), {{20, 4}}})};
    case Icon::Chamfer: return {poly({{4, 20}, {4, 10}, {10, 4}, {20, 4}})};
    case Icon::LinearPattern: return {rect(3, 9, 5, 6), rect(9.5, 9, 5, 6), rect(16, 9, 5, 6)};
    case Icon::CircularPattern:
      return {dashed(circle(12, 12, 8)), circle(12, 4, 2), circle(18.9, 16, 2), circle(5.1, 16, 2)};
    case Icon::Model: return {poly({{4, 20}, {8, 20}, {19, 9}, {15, 5}, {4, 16}}, true)};
    case Icon::Procedure: return {rect(4, 5, 5, 5), rect(15, 14, 5, 5), poly({{9, 7.5}, {12, 7.5}, {12, 16.5}, {15, 16.5}})};
    case Icon::Template: return {rect(4, 4, 7, 7), rect(13, 4, 7, 7), rect(4, 13, 7, 7), rect(13, 13, 7, 7)};
    case Icon::Import: return {line(12, 3, 12, 15), poly({{7, 10}, {12, 15}, {17, 10}}), line(4, 20, 20, 20)};
    case Icon::Composite: return {rect(3, 3, 18, 18), rect(7, 7, 4, 4), rect(13, 13, 4, 4)};
    case Icon::Generate: return {rect(4, 4, 16, 16), line(4, 12, 20, 12), line(12, 4, 12, 20)};
    case Icon::Validate:
      return {poly({{12, 3}, {20, 6}, {20, 12}, {17, 17.5}, {12, 21}, {7, 17.5}, {4, 12}, {4, 6}}, true),
              poly({{8.5, 12}, {11, 14.5}, {15.5, 9.5}})};
    case Icon::Done: return {poly({{5, 12}, {10, 17}, {19, 7}})};
    case Icon::Select: return {poly({{5, 3}, {19, 10}, {13, 12}, {11, 18}}, true)};
    case Icon::Line: return {line(5, 19, 19, 5), dot(5, 19), dot(19, 5)};
    case Icon::Rectangle: return {rect(4, 6, 16, 12)};
    case Icon::Circle: return {circle(12, 12, 8), dot(12, 12)};
    case Icon::Arc: return {arc(12, 18, 8, 180, 360), dot(4, 18), dot(20, 18)};
    case Icon::Slot: return {capsule(8, 12, 16, 12, 4)};
    case Icon::Coincident: return {circle(8.5, 12, 3.5), circle(15.5, 12, 3.5), dot(12, 12)};
    case Icon::PointOnCurve: return {arc(12, 22, 10, 200, 340), dot(12, 12)};
    case Icon::Horizontal: return {line(4, 12, 20, 12), dot(4, 12), dot(20, 12)};
    case Icon::Vertical: return {line(12, 4, 12, 20), dot(12, 4), dot(12, 20)};
    case Icon::Parallel: return {line(6, 19, 13, 5), line(11, 19, 18, 5)};
    case Icon::Perpendicular: return {line(4, 19, 20, 19), line(12, 19, 12, 5)};
    case Icon::Tangent: return {circle(10, 13, 6), line(3, 7, 21, 7)};
    case Icon::Equal: return {line(5, 9, 19, 9), line(5, 15, 19, 15)};
    case Icon::Symmetric: return {dashed(line(12, 3, 12, 21)), dot(6, 12), dot(18, 12)};
    case Icon::Fix:
      return {rect(6, 11, 12, 9), join({{{8, 11}}, arc_points(12, 9, 4, 180, 360), {{16, 11}}})};
    case Icon::Distance:
      return {line(5, 19, 19, 5), poly({{5, 14}, {5, 19}, {10, 19}}), poly({{14, 5}, {19, 5}, {19, 10}})};
    case Icon::HorizontalDistance:
      return {line(4, 8, 4, 16), line(20, 8, 20, 16), line(4, 12, 20, 12), poly({{7, 9}, {4, 12}, {7, 15}}),
              poly({{17, 9}, {20, 12}, {17, 15}})};
    case Icon::VerticalDistance:
      return {line(8, 4, 16, 4), line(8, 20, 16, 20), line(12, 4, 12, 20), poly({{9, 7}, {12, 4}, {15, 7}}),
              poly({{9, 17}, {12, 20}, {15, 17}})};
    case Icon::Angle: return {line(4, 20, 20, 20), line(4, 20, 16, 6), arc(4, 20, 9, -50, 0)};
    case Icon::Radius: return {circle(12, 12, 8), line(12, 12, 17.7, 6.3), dot(12, 12)};
    case Icon::Diameter: return {circle(12, 12, 8), line(6.3, 17.7, 17.7, 6.3)};
    case Icon::Construction: return {dashed(line(5, 19, 19, 5))};
    case Icon::Delete:
      return {line(5, 7, 19, 7), poly({{10, 7}, {10, 4}, {14, 4}, {14, 7}}), poly({{7, 7}, {8, 20}, {16, 20}, {17, 7}})};
    case Icon::Cancel: return {line(6, 6, 18, 18), line(18, 6, 6, 18)};
    case Icon::Bake: return {poly({{4, 19}, {12, 5}, {20, 19}}, true), poly({{8, 12}, {16, 12}, {12, 19}}, true)};
    case Icon::Unbake: return {dashed(poly({{4, 19}, {12, 5}, {20, 19}}, true)), dot(12, 14)};
    case Icon::Drawing:
      return {poly({{5, 3}, {15, 3}, {19, 7}, {19, 21}, {5, 21}}, true), poly({{15, 3}, {15, 7}, {19, 7}}),
              line(8, 13, 16, 13), line(8, 17, 13, 17)};
    case Icon::View: return {rect(3, 5, 18, 14), poly({{12, 8}, {16, 10}, {12, 12}, {8, 10}}, true), line(8, 10, 8, 14),
                             line(16, 10, 16, 14), poly({{8, 14}, {12, 16}, {16, 14}}), line(12, 12, 12, 16)};
    case Icon::Export: return {line(12, 15, 12, 3), poly({{7, 8}, {12, 3}, {17, 8}}), poly({{4, 14}, {4, 20}, {20, 20}, {20, 14}})};
    case Icon::Close: return {rect(4, 4, 16, 16), line(9, 9, 15, 15), line(15, 9, 9, 15)};
    case Icon::Regenerate: return {arc(12, 12, 8, 0, 315), poly({{20, 4}, {20, 9}, {15, 9}})};
    case Icon::Undo:
      return {poly({{9, 14}, {4, 9}, {9, 4}}), join({{{4, 9}}, arc_points(15, 14, 5, 270, 450), {{12, 19}}})};
    case Icon::Redo:
      return {mirrored(poly({{9, 14}, {4, 9}, {9, 4}})),
              mirrored(join({{{4, 9}}, arc_points(15, 14, 5, 270, 450), {{12, 19}}}))};
    case Icon::FitAll:
      return {poly({{4, 9}, {4, 4}, {9, 4}}), poly({{15, 4}, {20, 4}, {20, 9}}), poly({{4, 15}, {4, 20}, {9, 20}}),
              poly({{20, 15}, {20, 20}, {15, 20}})};
    case Icon::Error: return {circle(12, 12, 9), line(9, 9, 15, 15), line(15, 9, 9, 15)};
    case Icon::Warning: return {poly({{12, 4}, {21, 20}, {3, 20}}, true), line(12, 10, 12, 14), dot(12, 17)};
    case Icon::ChevronLeft: return {poly({{15, 6}, {9, 12}, {15, 18}})};
    case Icon::ChevronRight: return {poly({{9, 6}, {15, 12}, {9, 18}})};
    case Icon::ChevronUp: return {poly({{6, 15}, {12, 9}, {18, 15}})};
    case Icon::ChevronDown: return {poly({{6, 9}, {12, 15}, {18, 9}})};
    case Icon::Browser: return {line(4, 5, 20, 5), line(4, 10, 14, 10), line(8, 15, 20, 15), line(8, 20, 16, 20)};
    case Icon::Variables:
      return {poly({{8, 4}, {6, 8}, {6, 16}, {8, 20}}), poly({{16, 4}, {18, 8}, {18, 16}, {16, 20}}), line(10, 9, 14, 15),
              line(14, 9, 10, 15)};
  }
  return {dot(12, 12)};
}

constexpr std::array kIcons = {
    Icon::Volume, Icon::Surface, Icon::KeepOut, Icon::Link, Icon::Sketch, Icon::Plane, Icon::Axis, Icon::Point,
    Icon::Extrude, Icon::Revolve, Icon::Sweep, Icon::Loft, Icon::Shell, Icon::Fillet, Icon::Chamfer,
    Icon::LinearPattern, Icon::CircularPattern, Icon::Model, Icon::Procedure, Icon::Template, Icon::Import,
    Icon::Composite, Icon::Generate, Icon::Validate, Icon::Done, Icon::Select, Icon::Line, Icon::Rectangle,
    Icon::Circle, Icon::Arc, Icon::Slot, Icon::Coincident, Icon::PointOnCurve, Icon::Horizontal, Icon::Vertical,
    Icon::Parallel, Icon::Perpendicular, Icon::Tangent, Icon::Equal, Icon::Symmetric, Icon::Fix, Icon::Distance,
    Icon::HorizontalDistance, Icon::VerticalDistance, Icon::Angle, Icon::Radius, Icon::Diameter, Icon::Construction,
    Icon::Delete, Icon::Cancel, Icon::Bake, Icon::Unbake, Icon::Drawing, Icon::View, Icon::Export, Icon::Close,
    Icon::Regenerate, Icon::Undo, Icon::Redo, Icon::FitAll, Icon::Error, Icon::Warning, Icon::ChevronLeft,
    Icon::ChevronRight, Icon::ChevronUp, Icon::ChevronDown, Icon::Browser, Icon::Variables,
};

constexpr std::array<std::string_view, kIcons.size()> kNames = {
    "volume", "surface", "keep_out", "link", "sketch", "plane", "axis", "point", "extrude", "revolve", "sweep",
    "loft", "shell", "fillet", "chamfer", "linear_pattern", "circular_pattern", "model", "procedure", "template",
    "import", "composite", "generate", "validate", "done", "select", "line", "rectangle", "circle", "arc", "slot",
    "coincident", "point_on_curve", "horizontal", "vertical", "parallel", "perpendicular", "tangent", "equal",
    "symmetric", "fix", "distance", "horizontal_distance", "vertical_distance", "angle", "radius", "diameter",
    "construction", "delete", "cancel", "bake", "unbake", "drawing", "view", "export", "close", "regenerate",
    "undo", "redo", "fit_all", "error", "warning", "chevron_left", "chevron_right", "chevron_up", "chevron_down",
    "browser", "variables",
};

}  // namespace

std::span<const Icon> all_icons() { return kIcons; }

std::string_view to_string(Icon icon) {
  const auto i = static_cast<std::size_t>(icon);
  return i < kNames.size() ? kNames.at(i) : std::string_view("unknown");
}

const std::vector<IconStroke>& icon_strokes(Icon icon) {
  static const auto table = [] {
    std::array<Strokes, kIcons.size()> t;
    for (std::size_t i = 0; i < kIcons.size(); ++i) t.at(i) = make(kIcons.at(i));
    return t;
  }();
  const auto i = static_cast<std::size_t>(icon);
  static const Strokes fallback = {dot(12, 12)};
  return i < table.size() ? table.at(i) : fallback;
}

}  // namespace vs::app

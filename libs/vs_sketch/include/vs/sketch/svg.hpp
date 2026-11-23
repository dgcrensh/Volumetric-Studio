#pragma once

// SVG import (S1, decision 0040): the outlines of an SVG file's paths and
// shapes as sketch geometry that can be constrained and made into profiles.
//
// Only geometry is read. Fills, strokes, styles, clip paths, masks and
// filters are ignored; `text`, `image` and `foreignObject` are skipped with
// `sketch.import.unsupported`. What comes in:
//   - `path` (M L H V Z C S Q T A, absolute and relative): lines, cubic
//     Béziers (quadratics raised exactly), and arcs. An elliptical arc that
//     is circular after every transform becomes a sketch arc; any other is
//     approximated by Béziers within the minimum feature size
//     (`sketch.import.ellipse_approximated`, one warning per import).
//   - `rect` (rounded corners too), `circle`, `ellipse`, `line`, `polyline`
//     and `polygon` as the equivalent path; a circle (or an ellipse that is
//     circular after its transforms) becomes a sketch circle.
//   - `g`, `a`, `switch`, nested `svg` and `use` (of any element, `symbol`
//     included), with every transform and viewBox flattened. Elements inside
//     `defs`, `symbol`, `clipPath`, `mask`, `pattern` and `marker` are drawn
//     only through `use`; `display="none"` (attribute or style) hides one.
//
// Units: the outermost element's width and height in mm, cm, in, pt or pc
// set the scale through its viewBox; bare numbers and px use
// `pixels_per_inch`. Y is flipped so the drawing reads the same way up, with
// the viewport's lower left corner at the sketch origin.
//
// Joints: curve ends closer than `gap_mm` share one point entity, across
// subpaths and elements too, so outlines drawn in pieces join. Curves that
// don't lie on a closed loop (open subpaths, loose tails) come in as
// construction geometry with `sketch.import.open`, so nothing open reaches a
// profile but nothing is lost either. Segments shorter than the gap are
// dropped.

#include <filesystem>
#include <string_view>
#include <vector>

#include "vs/core/settings.hpp"
#include "vs/sketch/sketch.hpp"

namespace vs::sketch {

struct SvgImportOptions {
  // How many px (and user units without a unit) make an inch.
  double pixels_per_inch = 96;
  // Multiplies every length after units are converted.
  double scale = 1;
  // Ends closer than this (mm) are joined.
  double gap_mm = 0.01;
  // Adds Tangent where two curves meet smoothly (their directions within
  // kSmoothJointAngle): a line or arc with an arc, or a Bézier with a line,
  // arc or Bézier. Off, and always off when `fix` is on.
  bool smooth_joints = true;
  // Holds the imported geometry where it is: Fix on every point (curve ends
  // and Bézier handles) and every circle, and on every arc a horizontal or
  // vertical distance from its start to its centre (with its ends fixed,
  // that fixes it without a redundant constraint, half circles included).
  bool fix = false;
};

// How far apart two curves' directions may be at a joint (radians) and still
// count as smooth.
inline constexpr double kSmoothJointAngle = 0.01;

struct SvgImport {
  // Every line, arc, circle and Bézier added, in document order.
  std::vector<Id> curves;
  // Those of `curves` that came in as construction geometry (not on a loop).
  std::vector<Id> open;
  // The constraints added (tangents, or fixes and radii).
  std::vector<Id> constraints;
  // Warnings (`sketch.import.*`).
  Diagnostics diagnostics;
};

// Adds the outlines of the SVG document `svg` to `sketch`. On error
// (`sketch.import.malformed`, `.empty`, `.option`) the sketch is unchanged.
Result<SvgImport> import_svg(std::string_view svg, Sketch& sketch,
                             const SvgImportOptions& options = {},
                             const core::ProductSettings& settings =
                                 core::ProductSettings::defaults());

// The same, reading a file (`sketch.import.file` when it can't be read).
Result<SvgImport> import_svg_file(const std::filesystem::path& path, Sketch& sketch,
                                  const SvgImportOptions& options = {},
                                  const core::ProductSettings& settings =
                                      core::ProductSettings::defaults());

}  // namespace vs::sketch

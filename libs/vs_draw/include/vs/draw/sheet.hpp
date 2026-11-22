#pragma once

// A drawing sheet as plain 2D primitives (L6), and its files.
//
// Coordinates are sheet millimetres from the lower left corner, y up. Every
// primitive has a pen (what it is, which sets its line weight, dash and
// colour in every writer) and the id of what it came from (a volume, a
// connection surface, a dimension; null for the border and title block), so
// a viewer can pick it.
//
// The writers write numbers with fixed precision (0.001 mm; PDF points to
// 0.001), so the same sheet gives the same bytes on every platform:
//
//   to_svg()  SVG 1.1, mm units, one CSS class per pen;
//   to_pdf()  PDF 1.4, one page, vector paths and Helvetica (WinAnsi;
//             characters outside it become '?', the diameter sign 'Ø');
//   to_dxf()  AutoCAD R12 ASCII DXF, one layer per pen (LINE, POLYLINE,
//             SOLID, TEXT; the diameter sign "%%c").

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/core/vec.hpp"

namespace vs::draw {

using core::dvec2;
using core::Id;

enum class Pen {
  Border,      // the sheet's frame, the title block and its text
  Visible,     // contents' visible edges
  Hidden,      // contents' hidden edges (dashed)
  Boundary,    // boundaries (phantom)
  Surface,     // connection surfaces' outlines (dash-dot)
  Dimension,   // stable dimensions and view labels
  Disposable,  // dimensions on generated contents
};
std::string_view to_string(Pen pen);

struct SheetPath {
  Pen pen = Pen::Visible;
  Id source;
  std::vector<dvec2> points;  // at least two
  bool closed = false;
  bool filled = false;  // a closed, filled polygon (arrowheads)
};

enum class TextAlign { Left, Centre, Right };

struct SheetText {
  Pen pen = Pen::Dimension;
  Id source;
  dvec2 at;  // the baseline's left, centre or right end, per `align`
  double height = 3.5;
  bool vertical = false;  // turned a quarter turn counter-clockwise
  TextAlign align = TextAlign::Left;
  std::string text;  // UTF-8
};

struct Sheet {
  double width = 420.0;
  double height = 297.0;
  std::string title;
  std::vector<SheetPath> paths;
  std::vector<SheetText> texts;
};

// About how wide `text` is in Helvetica at `height` (cap height ~ 0.72 em),
// for laying text out; every writer aligns with it.
double text_width(std::string_view text, double height);

std::string to_svg(const Sheet& sheet);
std::string to_pdf(const Sheet& sheet);
std::string to_dxf(const Sheet& sheet);

// Writes `sheet` in the format its extension names (.svg, .pdf, .dxf).
// Codes: draw.file.format (another extension), draw.file.write.
core::Diagnostics write_sheet(const Sheet& sheet, const std::filesystem::path& path);

}  // namespace vs::draw

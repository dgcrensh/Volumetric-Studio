#pragma once

// vs_draw: drawings (L6, decision 0038; plan docs/plans/L6-drawings.md).
// A drawing's definition lives in the document (vs_model, document.hpp);
// this module computes what it shows from an evaluation:
//
//   - view.hpp: view frames, and projecting boundaries, connection surfaces
//     and contents (B-rep or mesh) into 2D lines, contents with hidden
//     lines removed;
//   - drawing.hpp: a whole sheet with its views, dimensions measured from
//     anchors (stable on boundaries and surfaces, disposable on contents)
//     and a title block; and a standard drawing to start from;
//   - sheet.hpp: the sheet as 2D primitives, written as SVG, PDF or DXF.

#include <string_view>

namespace vs::draw {

// The module's name, so the skeleton has a symbol to link and test.
std::string_view module_name();

}  // namespace vs::draw

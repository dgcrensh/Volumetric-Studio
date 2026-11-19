#pragma once

// JSON form of a sketch.
//
// The format keeps every id, so a sketch written and read back is equal to
// the original (operator==) and references into it stay valid. Numbers are
// written with enough digits to round-trip exactly.
//
//   {
//     "format": "vstudio.sketch", "version": 1,
//     "id": "<32 hex>", "name": "...",
//     "plane": {"origin": [x, y, z], "x_axis": [...], "y_axis": [...]},
//     "entities": [
//       {"id": "...", "type": "point", "position": [x, y]},
//       {"id": "...", "type": "line", "start": "<point id>", "end": "<point id>"},
//       {"id": "...", "type": "circle", "center": "<point id>", "radius": r},
//       {"id": "...", "type": "arc", "center": "...", "start": "...", "end": "..."}
//     ],
//     "constraints": [
//       {"id": "...", "type": "distance", "refs": ["...", "..."],
//        "value": 10.0, "expression": "width / 2"}
//     ]
//   }
//
// Entities may also carry "construction": true and "name"; constraints may
// carry "name". Optional keys are omitted at their defaults. Reading checks
// the structure and value types ("sketch.json.*"); whether references make
// sense is Sketch::validate()'s job.

#include <string>
#include <string_view>

#include "vs/sketch/sketch.hpp"

namespace vs::sketch {

inline constexpr int kSketchFormatVersion = 1;

// Pretty-printed JSON (two-space indent, trailing newline).
std::string to_json(const Sketch& sketch);

Result<Sketch> sketch_from_json(std::string_view text);

}  // namespace vs::sketch

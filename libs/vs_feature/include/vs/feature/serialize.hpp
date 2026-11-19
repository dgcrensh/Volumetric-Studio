#pragma once

// JSON form of a feature tree: what `vstudio-cli build` reads.
//
//   {
//     "format": "vstudio.features", "version": 1,
//     "variables": [{"id": "<32 hex>", "name": "width", "expression": "40 mm"}],
//     "features": [
//       {"id": "...", "type": "sketch", "plane": <plane ref>,
//        "sketch": { ...a vstudio.sketch document (vs_sketch serialize.hpp)... }},
//       {"id": "...", "type": "datum_plane",
//        "offset": {"base": <plane ref>, "distance": <param>}},
//       {"id": "...", "type": "datum_plane",
//        "through_points": {"a": <point ref>, "b": <point ref>, "c": <point ref>}},
//       {"id": "...", "type": "datum_plane",
//        "axis_angle": {"axis": <axis ref>, "base": <plane ref>, "angle": <param>}},
//       {"id": "...", "type": "datum_axis", "through_points": {"a": ..., "b": ...}},
//       {"id": "...", "type": "datum_axis", "plane_intersection": {"a": ..., "b": ...}},
//       {"id": "...", "type": "datum_axis", "along": <axis ref>},
//       {"id": "...", "type": "datum_point", "at": {"x": <param>, "y": ..., "z": ...}},
//       {"id": "...", "type": "datum_point", "at_ref": <point ref>},
//       {"id": "...", "type": "datum_point", "axis_plane": {"axis": ..., "plane": ...}},
//       {"id": "...", "type": "extrude", "sketch": "<id>", "operation": "add" | "cut",
//        "extent": "blind" | "through_all" | "up_to_plane" | "symmetric",
//        "depth": <param>, "up_to": <plane ref>, "draft": <param>, "reverse": false},
//       {"id": "...", "type": "revolve", "sketch": "<id>", "operation": "add",
//        "axis": <axis ref>, "angle": <param>, "reverse": false},
//       {"id": "...", "type": "sweep", "profile": "<sketch id>", "path": "<sketch id>",
//        "operation": "add", "orientation": "follow_path" | "fixed"},
//       {"id": "...", "type": "loft", "sections": ["<sketch id>", ...],
//        "operation": "add", "ruled": false},
//       {"id": "...", "type": "shell", "open": [<face ref>, ...], "thickness": <param>,
//        "outward": false},
//       {"id": "...", "type": "fillet", "edges": [<edge ref>, ...], "radius": <param>},
//       {"id": "...", "type": "chamfer", "edges": [<edge ref>, ...], "distance": <param>},
//       {"id": "...", "type": "linear_pattern", "features": ["<id>", ...],
//        "direction": <axis ref>, "count": <param>, "spacing": <param>, "reverse": false},
//       {"id": "...", "type": "circular_pattern", "features": ["<id>", ...],
//        "axis": <axis ref>, "count": <param>, "angle": <param>, "reverse": false}
//     ]
//   }
//
// Every feature may also carry "name" and "suppressed". Optional keys are
// omitted at their defaults when writing.
//
//   <param>      a number (the value), or {"value": 10, "expression": "width / 2"};
//                reading also accepts a bare string as the expression
//   <plane ref>  {"principal": "xy" | "yz" | "zx"} | {"feature": "<id>"} |
//                {"face": {"feature": "<id>", "name": "end"}}
//   <axis ref>   {"principal": "x" | "y" | "z"} | {"feature": "<id>"} |
//                {"sketch_entity": {"sketch": "<id>", "entity": "<id>"}}
//   <point ref>  {"feature": "<id>"} | {"sketch_entity": {"sketch": "<id>", "entity": "<id>"}}
//   <face ref>   {"feature": "<id>", "name": "end"}
//   <edge ref>   {"face": <face ref>} (every edge of the face) |
//                {"face": <face ref>, "and": <face ref>} (where the two meet)
//
// Reading checks structure and types ("feature.json.*"); the variables are
// checked as if authored, so a file whose variables loop is rejected with
// "feature.variable.loop". A tree written and read back is equal to the
// original.

#include <string>
#include <string_view>

#include "vs/feature/tree.hpp"

namespace vs::feature {

inline constexpr int kFeatureFormatVersion = 1;

std::string to_json(const FeatureTree& tree);
Result<FeatureTree> feature_tree_from_json(std::string_view text);

}  // namespace vs::feature

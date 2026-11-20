#pragma once

// A composite whose children a procedure generates (L4), shared by the
// suites.
//
//   root layout     points for two boxes
//     Base          box (0, 0, -10) .. (60, 40, 0)     leaf, filled
//     Rack          box (0, 0, 0) .. (60, 40, 10)      composite with a
//                   generator (the caller's graph) and one template:
//       Cell        parametric, authored for the box (0, 0, 0) .. (20, 20, 10)
//                   (origin 0): a rectangle on its "+z" face covering it,
//                   extruded down by parameter `depth` (default 10: full)
//
//   Seat  (root)    bonded, z = 0, x 2..18, y 2..18: Base and Rack. A
//                   generator passes it to the cell at the origin.
//
// vs_model can't make graphs; vs_proc's tests give the generator (a grid
// of 3 x 2 cells, say), and vs_model's tests generate layouts by hand.

#include <string>

#include "vs/core/box.hpp"
#include "vs/model/document.hpp"
#include "vs/model/project.hpp"

namespace rack {

using vs::core::Id;

struct Rack {
  vs::model::Project project;
  Id base, rack;      // volumes
  Id seat;            // the connection surface
  Id cell;            // the template's id
  Id depth;           // the template's parameter
};

// `generator` is the Rack's graph JSON; any non-empty text makes it a
// generating composite.
Rack make_rack(std::string generator = "{}");

// The Cell template's tree for a cell of `size` authored at the origin,
// referring to its boundary by `template_id`.
vs::model::ParametricContents cell_tree(const Id& template_id, const vs::core::dvec3& size);

}  // namespace rack

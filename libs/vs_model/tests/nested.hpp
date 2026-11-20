#pragma once

// A document nested three levels deep (L3), shared by the suites.
//
//   root layout     variable w = 40; points for two boxes
//     Base          box (0, 0, -10) .. (w, 20, 0)             leaf
//     Housing       box (0, 0, 0) .. (w, 20, 20)              composite,
//                   parameter width = w
//       Left        box (0, 0, 0) .. (20, 20, 20)             composite
//         Core      box (0, 0, 0) .. (20, 20, 10)             leaf
//         Cap       box (0, 0, 10) .. (20, 20, 20)            leaf
//       Right       box (20, 0, 0) .. (width, 20, 20)         leaf
//
//   Seat    (root)     bonded, z = 0, x 0..20: Base and Housing;
//                      Housing passes it to Left, Left to Core
//   Middle  (Housing)  bonded, x = 20, z 0..10: Left and Right;
//                      Left passes it to Core
//   Joint   (Left)     bonded, z = 10: Core and Cap
//
// Every leaf is a placeholder; tests give them contents as they need
// (fill() fills a box exactly).

#include "vs/core/box.hpp"
#include "vs/model/document.hpp"
#include "vs/model/project.hpp"

namespace nested {

using vs::core::Id;

struct Nested {
  vs::model::Project project;
  Id base, housing, left, right, core, cap;  // volumes
  Id seat, middle, joint;                    // connection surfaces
  Id w;                                      // the root variable
  Id width;                                  // Housing's parameter
  Id housing_split;  // Housing's layout point (20, 20, 20): Left's corner and Right's
  Id left_mid;       // Left's layout point (20, 20, 10): Core's top corner
};

Nested make_nested();

// The volume's box in the nested document as made (w = 40).
vs::core::dbox3 box_of(const Nested& n, const Id& volume);

// Parametric contents filling `box` exactly: a rectangle on a datum plane at
// its bottom, extruded to its top.
vs::model::ParametricContents fill(const vs::core::dbox3& box);

// Fills every leaf with fill().
void fill_leaves(Nested& n);

}  // namespace nested

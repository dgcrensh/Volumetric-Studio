#pragma once

// Test parts shared by the vs_feature suites.

#include <string>

#include "vs/feature/rebuild.hpp"
#include "vs/feature/tree.hpp"
#include "vs/kernel/shape.hpp"
#include "vs/sketch/sketch.hpp"

namespace parts {

using vs::core::dvec2;
using vs::core::Id;
using vs::core::ProductSettings;
using vs::feature::FeatureTree;
using vs::sketch::Sketch;

inline const ProductSettings kSettings = ProductSettings::defaults();

// An axis-aligned rectangle: corners p[0] (lo) counter-clockwise, lines
// l[0] bottom, l[1] right, l[2] top, l[3] left.
struct Rect {
  Id p[4];
  Id l[4];
};
Rect add_rectangle(Sketch& sketch, dvec2 lo, dvec2 hi);

// Fixes p[0] and constrains the rectangle's width and height, each by
// expression when given, else to its current size.
void constrain_rectangle(Sketch& sketch, const Rect& r, const std::string& width = {},
                         const std::string& height = {});

// Sets the expression of a dimensional constraint.
void set_expression(Sketch& sketch, const Id& constraint, const std::string& expression);

// A plate driven by three variables: `width` by `width / 2`, `thickness`
// thick, with a through hole of diameter `hole_d` cut from a sketch on its
// top face, centred at (width / 2, width / 4).
struct Plate {
  FeatureTree tree;
  Id width, thickness, hole_d;            // variables
  Id base_sketch, base, hole_sketch, hole;  // features
  Rect rect;
};
Plate make_plate(double width = 60, double thickness = 8, double hole_d = 12);
double plate_volume(double width, double thickness, double hole_d);

// The solid's volume (fails the test when it can't be measured).
double volume(const vs::kernel::Solid& solid);

// The codes of a diagnostics list, for readable assertions.
std::string codes(const vs::core::Diagnostics& diagnostics);

}  // namespace parts

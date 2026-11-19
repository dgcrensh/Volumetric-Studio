#pragma once

// Canonical sketches for the solver and profile tests. Each starts from a
// deliberately rough guess, so the solver has real work to do, and records
// the ids the tests check.

#include <vector>

#include "vs/sketch/sketch.hpp"

namespace vs::sketch::test {

// A w × h rectangle with its lower-left corner fixed at the origin.
// `dimensioned_width = false` leaves the width free (one degree of freedom).
struct Rectangle {
  Sketch sketch;
  Id p00, p10, p11, p01;              // corners, counter-clockwise from the origin
  Id bottom, right, top, left;        // lines
  Id h_bottom, h_top, v_right, v_left, fix, width, height;  // constraints
};
Rectangle rectangle(double w, double h, bool dimensioned_width = true);

// A slot: two half-circle ends of radius r whose centres are `length` apart
// on a horizontal line, joined by two tangent lines. The left centre is
// fixed at the origin.
struct Slot {
  Sketch sketch;
  Id c1, c2;                // arc centres
  Id left_arc, right_arc;   // arcs
  Id top, bottom;           // lines
  Id top_left, top_right, bottom_left, bottom_right;  // line end points
};
Slot slot(double length, double r);

// A gear-like outline with `teeth` square-ish teeth: points alternate between
// a root circle and a tip circle, each placed by an angle from the centre,
// joined by lines into one closed loop.
struct Gear {
  Sketch sketch;
  Id center;
  std::vector<Id> points;           // the outline's points, in order
  std::vector<dvec2> expected;      // where they belong
};
Gear gear(int teeth, double root_radius, double tip_radius);

// A round plate of radius `plate_r` with `holes` equal holes of diameter
// `hole_d` evenly spaced on a construction circle of radius `pitch_r`, the
// first on the positive x axis.
struct BoltCircle {
  Sketch sketch;
  Id center;
  Id plate;
  Id pitch;                         // construction circle
  std::vector<Id> holes;            // hole circles
  std::vector<Id> hole_centers;
};
BoltCircle bolt_circle(double plate_r, double pitch_r, int holes, double hole_d);

// True when `a` and `b` hold the same entities and constraints and their
// point positions and radii agree within `eps` mm. PlaneGCS orders some of
// its work by pointer, so two solves of one sketch can differ in the last
// bits; geometry is compared with a tolerance, everything else exactly.
bool same_geometry(const Sketch& a, const Sketch& b, double eps = 1e-9);

}  // namespace vs::sketch::test

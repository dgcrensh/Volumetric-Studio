#pragma once

// Profile extraction: closed loops of sketch geometry, nested into outer
// loops and holes, and the planar faces they bound.
//
// Only non-construction lines, arcs and circles take part. Curve ends are
// joined when they share a point entity or lie within the linear tolerance
// of each other. Every joint must join exactly two curve ends:
//
//   - an end that meets nothing is "sketch.profile.open";
//   - three or more ends at one joint is "sketch.profile.branching";
//   - two loops that cross each other are "sketch.profile.loops_cross".
//
// Loops nest by containment. A loop inside an even number of others
// (0, 2, ...) is an outer loop; one inside an odd number is a hole in the
// loop directly around it. So an island inside a hole is its own profile.
// Outer loops run counter-clockwise in sketch coordinates and holes
// clockwise, so faces face along the sketch plane's normal.

#include <vector>

#include "vs/core/settings.hpp"
#include "vs/kernel/shape.hpp"
#include "vs/sketch/sketch.hpp"

namespace vs::sketch {

using core::ProductSettings;

// One curve of a loop, used forwards or reversed.
struct LoopEdge {
  Id entity;
  bool reversed = false;

  friend bool operator==(const LoopEdge&, const LoopEdge&) = default;
};

struct ProfileLoop {
  std::vector<LoopEdge> edges;  // in order around the loop
  double area = 0;              // enclosed area, mm², always positive
};

struct Profile {
  ProfileLoop outer;
  std::vector<ProfileLoop> holes;

  // Outer area minus the holes' areas.
  [[nodiscard]] double area() const;
};

// The sketch's profiles, largest outer loop first. Fails with the first
// "sketch.profile.*" error; a sketch with no closed geometry yields an empty
// list. Uses the sketch's current point positions: solve it first.
Result<std::vector<Profile>> extract_profiles(const Sketch& sketch,
                                              const ProductSettings& settings);

// The planar face for `profile`, placed on the sketch's plane.
Result<kernel::Face> make_face(const Sketch& sketch, const Profile& profile,
                               const ProductSettings& settings);

// Every profile of the sketch as a face, in extract_profiles() order.
Result<std::vector<kernel::Face>> make_faces(const Sketch& sketch,
                                             const ProductSettings& settings);

}  // namespace vs::sketch

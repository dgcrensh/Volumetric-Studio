#pragma once

// Offsetting sketch curves (decision 0041): new curves a constant distance
// to one side of a chain of lines, arcs and Béziers, or round a circle.
//
// The curves given are ordered into chains through the end points they
// share (or join by Coincident). A chain is open (two loose ends) or closed
// (a loop); a point where three or more of the given curves meet is
// refused (`sketch.offset.branching`). A circle is a closed chain of its
// own.
//
// Side: a positive distance offsets a closed chain outward (away from the
// area it encloses) and an open chain to the left of its direction of
// travel; a negative distance offsets the other way, so an inset is a
// negative distance. `offset_chains()` gives each chain's direction.
//
// Accuracy: lines, arcs and circles offset exactly. An offset arc shares
// its source's centre point, and so stays concentric when the source moves.
// The offset of a Bézier isn't a Bézier: it becomes as many cubic Béziers
// as keep every point within the minimum feature size of the true offset
// (Hermite fits that match the offset's ends and end tangents, split in
// half until close enough), joined smoothly to each other.
//
// Corners: where two curves meet at an angle, the offsets either overlap
// (the inside of the turn), where both are trimmed back to where they
// cross, or leave a gap (the outside), which `OffsetCorners` closes:
// `Extend` extends the two offsets until they meet, keeping a sharp corner
// sharp; `Round` adds an arc about the corner point (also a point the arc
// shares with the source), the true constant-distance offset. A corner
// beside a Bézier, or where extensions never meet, is always rounded.
//
// What is refused, leaving the sketch unchanged: a distance under the
// minimum feature size (`sketch.offset.distance`); an inset at or past a
// curve's radius of curvature, which would collapse or fold it, or a trim
// that consumes a curve (`sketch.offset.collapsed`, naming the source
// curve); a result that crosses itself or turns a loop inside out
// (`sketch.offset.self_crossing`). Untangling such loops is not attempted.
//
// The result is plain geometry with no constraints, and no link to its
// source beyond the shared centre points.

#include <span>
#include <vector>

#include "vs/core/settings.hpp"
#include "vs/sketch/sketch.hpp"

namespace vs::sketch {

enum class OffsetCorners { Extend, Round };

struct OffsetOptions {
  // How far, in mm. Positive: outward for a closed chain, to the left for
  // an open one. Negative: inward, or to the right.
  double distance = 1.0;
  OffsetCorners corners = OffsetCorners::Extend;
};

// One chain of curves in travel order.
struct OffsetChain {
  std::vector<Id> curves;
  // Whether each curve runs from its end to its start along the chain.
  std::vector<bool> reversed;
  bool closed = false;
};

// Orders `curves` (lines, arcs, Béziers, circles; anything else is
// `sketch.offset.not_curve`) into chains. An open chain starts at the loose
// end of the earliest of its curves in sketch order; a loop starts at the
// earliest curve's start and runs along it. `sketch.offset.empty` when no
// curve is given.
Result<std::vector<OffsetChain>> offset_chains(const Sketch& sketch, std::span<const Id> curves);

// Grows a selection to whole chains: every curve joined to a selected one
// through end points where exactly two curves meet (all of the sketch's
// curves counting, so a chain stops where it branches). Points and
// constraints among `ids` are dropped; curves come back in sketch order.
std::vector<Id> connected_curves(const Sketch& sketch, std::span<const Id> ids);

struct SketchOffset {
  // The curves added, chain by chain in travel order.
  std::vector<Id> curves;
  // The points added (curve ends and Bézier handles).
  std::vector<Id> points;
};

// Adds the offset of `curves` to `sketch`. On error the sketch is unchanged.
Result<SketchOffset> offset_curves(Sketch& sketch, std::span<const Id> curves,
                                   const OffsetOptions& options,
                                   const core::ProductSettings& settings =
                                       core::ProductSettings::defaults());

}  // namespace vs::sketch

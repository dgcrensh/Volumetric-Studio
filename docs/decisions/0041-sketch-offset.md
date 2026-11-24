# 0041: Offsetting sketch curves

Nov 24, 2026 · builds on S1 (decision 0040)

## Questions

1. How is the offset of a Bézier represented, when it isn't a Bézier?
2. What happens at corners?
3. Does the offset stay linked to its source?
4. What is refused?

## Decisions

**One function in vs_sketch, one tool in the app.** `offset_curves()`
(`vs/sketch/offset.hpp`) orders the chosen curves into chains through
shared end points (or Coincident), offsets each chain and adds the
result to the sketch, or changes nothing and returns
`sketch.offset.*`. A positive distance is outward for a closed chain and
to the left of travel for an open one, so an inset is a negative
distance. The app's Sketch tab has Offset (`sketch_offset`, group
Modify): `OffsetDialog` takes a distance, a side, the corners and
"Whole chains" (`connected_curves()`: the selection grown through end
points where exactly two curves meet), previews on the sketch like Import
SVG, and OK keeps one sketch step.

**Lines, arcs and circles are exact; Béziers are fitted.** An offset arc
or circle shares its source's centre point, so it stays concentric. The
offset of a cubic is a curve of degree ten in general (and not even
rational where it has cusps), so it is approximated by cubic Béziers:
Hermite fits that match the true offset's ends and end derivatives
(`B'(1 − dκ)`), halved until every point is within the minimum feature
size (1 µm) of the true offset, measured by projecting the fit's points
onto the source. Joints between fits are exact in position and tangent
direction. The cost is more curves on tightly bent sources: a quarter circle
drawn as one Bézier offsets into 2 to 4, a strongly S-shaped 50 mm curve
into 10 to 18 (offsets of 0.5 to 5 mm). Alternatives rejected:
polylines (lose smoothness and editability), arcs (biarcs: many joints,
each needing tangency, rejected for the import in 0040 for the same
reason), and an exact rational representation (not a sketch entity).

**Corners: trim inside, extend or round outside.** Where two curves meet
at an angle, their offsets overlap on the inside of the turn and are
trimmed back to where they cross (searched by sampling, refined by
Newton). On the outside they leave a gap: `Extend` (the default) extends
both until they meet, as a line, circle, or line and circle intersection,
keeping sharp corners sharp; `Round` adds an arc about the corner point,
the true constant-distance offset. A corner beside a Bézier is always
rounded, because extending a Bézier isn't well defined, and so is one
whose extensions never meet. A corner arc's centre is the source's corner
point.

**Unlinked geometry.** Dallas chose plain curves with no constraints and
no link beyond the shared centre points. A linked offset (regenerated
from the source after each solve) would fit Béziers poorly, since a fit
can't be a solver constraint, and couldn't carry constraints of its own.
It can be added later on the same geometry code.

**Refused, never untangled.** A distance under the minimum feature size
(`sketch.offset.distance`); an inset at or past a curve's radius of
curvature, a Bézier that would fold, or a trim that consumes a curve
(`sketch.offset.collapsed`, naming the source curve); a result that
crosses itself, or a loop that would turn inside out or grow when it
should shrink (`sketch.offset.self_crossing`). Three chosen curves at one
point are `sketch.offset.branching`; points are `.not_curve`. Untangling
self-crossing offsets (dropping the swallowed loops) is a bigger job left
for later.

## Tests

`libs/vs_sketch/tests/test_offset.cpp` (chains, rectangles both ways
with both corners, slots, circles, an open L, a line meeting an arc,
Béziers within 1 µm and joined smoothly, a loop of Béziers, corners
beside a Bézier, regular and star polygons nesting as one profile with a
hole, refusals); `apps/vs_app/tests/test_sketch_editor.cpp` and
`test_tool_catalog.cpp` (one undo step, availability); and
`test_offset_ui.cpp` (the dialog's preview, Cancel and OK).

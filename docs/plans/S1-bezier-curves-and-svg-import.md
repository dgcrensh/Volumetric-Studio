# S1 — Bézier curves and SVG import (plan)

Nov 23, 2026 · follows U1 (context toolbar) · decision record:
`docs/decisions/0040-bezier-curves.md`

Status: phases 1 to 3 done.

## Goal

Import the paths of an SVG file into a sketch as outlines that can be
constrained and turned into volumes. Fills, strokes and styles are
ignored; unclosed shapes never reach a profile. SVG curves are cubic and
quadratic Béziers and elliptical arcs, so sketches gain a native cubic
Bézier entity first.

## Options considered

| Option | Verdict |
| --- | --- |
| Flatten curves to lines | No: faceted walls, hundreds of faces, nothing to edit |
| Fit curves to arcs and lines (biarcs) | Later, as a "Convert to arcs" command: one curve becomes 4 to 40 arcs that need a tangent at every joint |
| Bézier as fixed reference geometry | Phase 1 of the chosen option (the import's "Fix imported geometry") |
| **Cubic Bézier entity whose control points are point entities** | **Chosen** (Dallas, 23 Nov) |
| General NURBS (knots, weights, any degree) | Only if a later need appears |

## Phases

1. **Bézier through the stack (done).** `sketch::Bezier` (four point
   ids), validation, JSON, labels; `BezierGeometry` (de Casteljau,
   derivative, exact area term, chord-bounded sampling); profiles sample
   Béziers like arcs; `kernel::BezierSegment` builds an exact
   `Geom_BezierCurve` edge (faces, sweeps, revolves, lofts);
   `kernel::surface_normals_at()`; `NamedFace::samples` names an extruded
   Bézier's side and finds it again by points on its surface (patterns,
   fillets, chamfers follow it); Tangent at a shared end with a line, arc
   or Bézier (handle collinearity); Fix fixes the four points; point on
   curve, Equal, Radius and length on a Bézier refused. The app draws,
   picks and deletes Béziers but can't create them yet.
2. **SVG import library and CLI (done).** Move the 3MF XML reader to `vs_core`;
   `vs_sketch/svg.hpp` (`import_svg()`), `sketch.import.*` diagnostics;
   `vstudio-cli sketch-import-svg <file.svg> [--out sketch.json]
   [--scale k] [--dpi n] [--gap mm] [--fix]`. Scope:

   | SVG | Handling |
   | --- | --- |
   | `path` M L H V Z | Lines; Z closes onto the subpath's start |
   | C S | Béziers; S reflects the previous handle |
   | Q T | Raised to cubics, exactly |
   | A | A circular arc when the radii are equal after transforms; else cubics within the minimum feature size (`sketch.import.ellipse_approximated`, warning) |
   | `rect` (rx/ry), `circle`, `ellipse`, `line`, `polyline`, `polygon` | As the equivalent path; `circle` becomes a sketch circle |
   | `g`, nested `svg`, `use` | Transforms flattened; `use` expanded |
   | fill, stroke, styles, clip paths, masks, filters | Ignored |
   | `text`, `image` | Skipped, `sketch.import.unsupported` |

   Units: width and height in mm, cm, in or pt set the scale; bare numbers
   and px use a px-per-inch value, 96 by default and set in the dialog
   (`--dpi`). Y is flipped. Ends closer than a gap (0.01 mm by default)
   merge; open subpaths come in as construction geometry with
   `sketch.import.open` (warning), so they can be recovered. Options:
   Keep smooth joints (tangent where handles are collinear, on), Fix
   imported geometry (off).
3. **App (done).** A Curve tool (`sketch_bezier`) in the Sketch context's Draw
   group, handle display and dragging; an Insert group with Import SVG
   (`import_svg`), its dialog previewing through `History::try_apply()`
   and one undo step; icons. Tests in `vs_app_core_tests` and a `[gui]`
   import-and-extrude test.

   As built: the Curve tool takes the control polygon in order (start,
   handle, handle, end) and chains like lines; only its ends snap. The
   import runs inside sketch mode, so it previews on the sketch through
   `SketchEditor::import_svg()` (each preview replaces the last with
   `undo_and_forget()`) and OK keeps one sketch step, which Finish hands
   to History with the rest of the sketch; `History::try_apply()` isn't
   involved. `MainWindow::import_svg(path)` is the entry point tests
   drive; Finish with the dialog open keeps what it shows.
4. **Curves in the solver.** Béziers as one-span PlaneGCS `BSpline`s;
   point on curve with Béziers; tangency to a circle or arc anywhere along
   one; dof and conflicts reported as for other curves.

## Out of scope

Bézier-sided layout boundaries carrying connection surfaces (those are
planes or cylinders). A boundary may have Bézier sides; no surface can
join one. No work planned (Dallas, 23 Nov).

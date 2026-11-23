# 0040: Bézier curves in sketches

Nov 23, 2026 · S1 phase 1 · plan: `docs/plans/S1-bezier-curves-and-svg-import.md`

## Questions

1. How do SVG curves become sketch geometry that can still be edited?
2. What does the solver need to know about them?
3. How are faces made by them named and found again?
4. What changes for files and hashes?

## Decisions

**A cubic Bézier entity whose control points are point entities.**
`sketch::Bezier { start, control1, control2, end }` is a fifth
`Geometry` alternative. SVG's cubics map onto it exactly and its
quadratics after an exact degree raise. Fitting arcs to curves (biarcs)
was rejected as the import: one curve becomes many small arcs, each joint
needing a tangent constraint to stay smooth, and dimensions on them mean
little. General NURBS were rejected for now: knots and weights are hard
to constrain and SVG doesn't need them.

**The solver sees points, not curves (until phase 4).** A Bézier adds no
unknowns: its shape is its four points, so Fix, Coincident, distances,
Symmetric and drags work on its handles unchanged. Tangent at a shared
end keeps the handle on the line's line, square to the arc's radius at the
joint, or in line with the other Bézier's handle; tangency anywhere else,
point on curve, Equal, Radius, Diameter and length on a Bézier are
`sketch.constraint.bad_form` until PlaneGCS's B-spline joins in phase 4.

**Profiles sample Béziers like arcs.** Equal steps in t, as many as keep
every chord within the minimum feature size
(`BezierGeometry::steps_within()`, from the bound on the second
derivative), so the open, branching, crossing and nesting rules apply
unchanged; a curve that loops over itself is `self_crossing`. Areas use
the exact closed form. Faces get exact `Geom_BezierCurve` edges
(`kernel::BezierSegment`); they skip the convex-polygon shortcut, so
`check_validity` always runs on them.

**Bézier side faces are named by samples.** Face references found faces
by plane or cylinder. An extruded Bézier's side is neither, so
`NamedFace::samples` holds three points on it (a quarter, half and three
quarters along the curve, on the sketch plane) with the face's outward
normals. A body face is on it when `kernel::surface_normals_at()` finds
every point within the linear tolerance of the face's underlying surface
(searched well past the trimmed face, so a split piece still matches),
facing the same way; planes and cylinders are skipped first. Patterns
move the samples with their copies, and fillets and chamfers find edges
by them. A sampled face is curved: a sketch or datum on it is
`feature.reference.wrong_kind`, the app lists it as a face but not as a
plane, and drawings don't anchor on it. Drafted extrusions don't name
Bézier sides, as for arcs.

**Files and hashes keep their bytes.** The sketch JSON gains a `bezier`
type; sketches without one write exactly what they did, so content
hashes and cache keys of existing documents are unchanged. A named face's
samples enter the cache key only when present.

**Limitation.** Connection surfaces are planes or cylinders, so a layout
boundary may have Bézier sides but no surface can join one. Out of scope
(Dallas).

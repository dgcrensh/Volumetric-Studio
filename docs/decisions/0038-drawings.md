# 0038: Drawings

Nov 22, 2026 · L6 · plan: `docs/plans/L6-drawings.md`

## Questions

1. Where does a drawing live, and what of it is saved?
2. What makes a dimension stable or disposable, and what can it drive?
3. How are views projected, for B-reps and meshes alike?
4. Which file formats, and whose writers?
5. Where does the code go in the module order?

## Decisions

**Only the definition is saved, in the document.** `Document::drawings`
holds each drawing's sheet (size, decimals), views (direction, shown
volumes, what is drawn, scale, position) and dimensions (kind, anchors,
offset), written to JSON only when set, so every earlier document, content
hash, cache key and golden value is unchanged. Nothing measured or
projected is stored: `vs_draw::draw()` computes the sheet from an
evaluation every time, so a drawing never goes out of date and shows
generated contents as they are. `layout_inputs()` strips drawings, so the
engine's layouts and cache keys never see them; editing a drawing
regenerates nothing.

**Stable on fixed inputs, disposable on contents.** An anchor is on the
layout origin, a volume's boundary (min, max, centre, a named boundary
face), a connection surface (min, max, centre, a bolt hole) or a volume's
contents (min, max, centre, a named face of parametric contents). The
first three are built from the layout, so a dimension between them means
the same after any regeneration: stable. A dimension with an anchor on
contents is disposable: measured again from every evaluation, written in
parentheses (the drafting convention for reference dimensions) in its own
pen and DXF layer, and dropped with `draw.dimension.lost` (a warning)
when its anchor is gone or no longer fits, where a stable one would be an
error. No dimension drives anything: they have no names expressions can
see, and `model.link.bad_target` and `model.configuration.target` already
refuse every id that isn't a drivable number (tested). The handles that
drive the layout stay in the 3D viewport (M16).

**Checked when authored, measured when drawn.** `check()` refuses what is
wrong without a build (`model.drawing.name`, `.sheet`, `.volume`, `.view`,
`.anchor`: a part its kind doesn't have, a hole of a surface that isn't a
bolted flange). Face names, planes facing the wrong way and circles seen
edge-on need a build, so `draw()` reports them (`draw.dimension.*`).
Deleting what a drawing shows is never refused: `prune_drawings()` drops
views' volumes and dimensions whose anchors are gone, and the app's
history calls it after every edit, as it does `prune_configurations()`.

**Our own hidden-line removal, for B-reps and meshes alike.** B-rep
contents are tessellated (`kernel::tessellate()`): their edges (seams and
degenerate edges left out) plus silhouettes found on each face's
triangulation, welded across seams by point. Meshes give borders, creases
over 60° and silhouettes. A point of an edge is hidden when a contents
triangle covering it, found through a uniform grid over the view, is
nearer the viewer by more than a tolerance; edges are sampled every 0.1
sheet mm, split into visible and hidden runs, hidden runs lying on a
visible line dropped, then simplified (Douglas-Peucker). Facets seen
exactly edge-on are neither occluders nor front faces, so a cylinder's
side silhouettes stay visible. Boundaries and connection surfaces are
drawn whole as outlines (phantom and dash-dot) and hide nothing: every
boundary contains its contents. OCCT's HLR was left out: it covers
B-reps only, so meshes would need this path anyway. Everything is FP64 in
a fixed order; threads split independent edges and are folded in order,
so the files are the same on any thread count (tested).

**SVG, PDF and DXF, written by us.** Each is a few hundred lines with
fixed-precision numbers, so files are identical on every platform and
need no dependency (a cloud session can't add one, decision 0005): SVG
1.1; PDF 1.4 with Helvetica in WinAnsi (the diameter sign as Ø) and
checked cross-reference offsets; DXF R12 ASCII with a layer per pen and
`%%c` for the diameter sign. A sheet has a border and a title block:
drawing and document names, the configuration (L5), each view's scale,
units, and what parentheses mean. `standard_drawing()` starts one: front,
top, right and isometric views third-angle at the largest standard scale
that fits, with the layout's width, height and depth as stable
dimensions between the boundaries at its extremes.

**A new library, `vs_draw`, after `vs_proc`.** It reads evaluations
(vs_eval) and its tests generate procedural contents, so it goes after
vs_proc and before vs_gpu. The model types are in vs_model (they are
saved and checked with the document); the app's commands and anchor
lists are in `vs_app_core`.

## Consequences

- Projects with drawings open in builds older than L6 without them
  (unknown keys are ignored); the next save drops them. Projects without
  drawings are unchanged.
- `SubjectKind::Drawing` is appended to vs_core's subject kinds;
  diagnostics about a drawing name its view or dimension first.
- `kernel::extent(shape, direction)` measures a shape's exact extent
  along any direction (OCCT's optimal box in a rotated frame), for
  min and max anchors.
- Visibility is sampled: a hidden run's ends are within a sample step
  (0.1 sheet mm) of the true crossing. Silhouettes are within the
  tessellation's deflection (0.02 sheet mm).
- Left out: sections and details, angular and radial dimensions,
  tolerances and GD&T, balloons and bills of materials, several sheets
  per drawing, embedded fonts.

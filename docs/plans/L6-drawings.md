# L6 — Drawings (plan)

Nov 22, 2026 · follows L5 (configurations and design tables) · decision
record: `docs/decisions/0038-drawings.md`

Status: in progress.

## Goal

The spec's "Ship" workflow ends with "export STEP, 3MF or drawings", and
its export section says: "Drawings can show generated geometry.
Dimensions placed on generated contents are disposable: they regenerate
with the volume and can't drive anything. Dimensions on boundaries and
connection surfaces are stable."

L6 gives a project 2D drawings: sheets of orthographic and isometric
views of its volumes, with dimensions, written as SVG, PDF and DXF. A
drawing shows whatever the evaluation shows, generated contents
included, so it never goes out of date: it is computed from an
evaluation every time, and only its definition is saved.

## What a drawing is

A drawing belongs to the document (`Document::drawings`, written to JSON
only when set, so every earlier document, hash and cache key is
unchanged). It holds:

- a sheet: a size in mm (A4 to A0 landscape, or any size), a name and how
  many decimals dimensions show;
- views: a direction (front, back, top, bottom, left, right, isometric),
  the volumes shown (none: all; a composite stands for everything inside
  it, generated children included), what is drawn (boundaries as
  phantom lines, contents with visible and optionally hidden edges,
  connection surfaces' outlines), a scale and where on the sheet its
  centre sits;
- dimensions: horizontal or vertical (along the view's axes) between two
  anchors, or a diameter of one, with an offset for where the dimension
  line sits.

Drawings change nothing that builds: cache keys, layouts and contents
never see them. Changing a drawing changes the content hash, like any
edit.

## Anchors: stable and disposable

An anchor is what a dimension measures from. It is on

| On | Parts | Stable? |
| --- | --- | --- |
| the layout's origin | — | stable |
| a volume's boundary | min or max along the dimension, centre, a named boundary face (`+x`, `<layout feature id>:end`, ...) | stable |
| a connection surface | min, max, centre (a cylindrical patch's axis), the k-th bolt hole | stable |
| a volume's contents | min or max along the dimension, centre, a named face of parametric contents (`<feature id>:<name>`) | disposable |

A dimension is disposable when either anchor is on contents. Boundaries
and connection surfaces are fixed inputs, built from the layout, so a
dimension on them means the same thing after any regeneration. Contents
are whatever the volume made: a disposable dimension is measured again
from each evaluation, and when its anchor is gone (the feature was
deleted, the contents were swapped to a procedure, generation failed) it
is dropped with a warning instead of an error.

"Can't drive anything": no drawing dimension drives anything, stable
ones included. They have no names expressions can use, links and
configurations can't target them (`model.link.bad_target`,
`model.configuration.target` already refuse any id that isn't a
drivable number), and the dimension handles that drive the layout stay
in the 3D viewport (M16). Disposable dimensions are written in
parentheses, the drafting convention for reference dimensions, and on
their own colour and DXF layer.

Measurement is exact where the geometry is: an extreme of a B-rep is
OCCT's optimal bounding box along the direction, a face's plane or a
cylinder's axis comes from the named face, a bolt hole's diameter is its
thread's ISO 273 clearance hole. Meshes give their vertices' extremes.

Linear dimensions need anchors that make sense along their direction: a
plane must face along it, a cylinder's axis must point at the viewer;
diameters need a circle facing the viewer. Otherwise the dimension is
refused at drawing time (`draw.dimension.not_parallel`,
`draw.dimension.not_circle`).

## Checks (vs_model)

`check()` adds `model.drawing.*`:

| Code | When |
| --- | --- |
| `model.drawing.name` | a drawing has no name, or another has the same name |
| `model.drawing.sheet` | a sheet size, view scale or decimals isn't usable |
| `model.drawing.volume` | a view shows a volume that isn't in the document |
| `model.drawing.view` | a dimension names a view of another drawing, or none |
| `model.drawing.anchor` | an anchor names nothing, the wrong kind of thing, or a part its kind doesn't have (a hole of a surface that isn't a bolted flange, a face of the origin, a diameter of a min) |

Names of faces aren't checked here: boundary faces need a build, and
contents faces are disposable by definition. `prune_drawings()` drops
what an edit removed (views' volumes, dimensions whose anchors' volume
or surface is gone), as `prune_configurations()` does for L5; the app's
history calls both.

## Projection (vs_draw)

A new library, `vs_draw`, after `vs_proc` (it reads evaluations and its
tests generate procedural contents):

```
vs_core → ... → vs_eval → vs_proc → vs_draw → vs_gpu
```

`project_view()` turns what a view shows into 2D polylines in view
coordinates (mm, x right, y up):

- **Boundaries** and **connection surfaces** are drawn whole, without
  hidden-line removal: they are envelopes and annotations, and every
  boundary contains its contents, so letting them occlude would hide
  everything.
- **Contents** are drawn with hidden-line removal among all contents in
  the view. B-rep contents are tessellated (`kernel::tessellate()`):
  their edges are B-rep edges (seams of closed surfaces left out) plus
  silhouettes from each face's triangulation. Mesh contents (procedural,
  imported, baked) contribute border edges, creases sharper than 30° and
  silhouettes.
- **Visibility** is our own ray test: every contents triangle is
  projected into a uniform grid over the view; a point of an edge is
  hidden when a triangle covering it lies nearer the viewer by more than
  a tolerance. Each edge is sampled every 0.1 sheet mm and split into
  visible and hidden runs, then chained and simplified (Douglas-Peucker,
  0.02 sheet mm). It works for B-reps and meshes alike, is deterministic
  (plain FP64 arithmetic, fixed order, optional thread pool folded in
  order) and handles a 100 mm gyroid.

OCCT's HLR was considered and left out: it covers B-reps only, so meshes
would need their own path anyway, and it costs another toolkit. A
tessellated silhouette is within the tessellation's deflection, set from
the view's scale (0.02 sheet mm).

## Sheets and files (vs_draw)

`make_drawing(evaluation, drawing id, options)` returns a `DrawingResult`:
the `Sheet` (polylines, filled arrows and text, each with a style and the
id it came from), every dimension's measured value and whether it is
stable, and `draw.*` diagnostics:

| Code | Severity | When |
| --- | --- | --- |
| `draw.drawing.missing` | error | no drawing of that id or name |
| `draw.view.empty` | warning | a view shows nothing (no volume built) |
| `draw.contents.stale` | warning | a shown volume's contents aren't current (stale or failed: its last valid contents are drawn, if any) |
| `draw.dimension.unresolved` | error | a stable anchor couldn't be found (its boundary failed, no such face, no such hole) |
| `draw.dimension.not_parallel` | error, or `lost` | a plane not facing along the dimension, or a cylinder not facing the viewer |
| `draw.dimension.not_circle` | error, or `lost` | a diameter of something that isn't a circle facing the viewer |
| `draw.dimension.lost` | warning | a disposable anchor is gone; the dimension isn't drawn |

Each sheet has a border and a title block: the drawing's and document's
names, the configuration (L5), the scale of each view, "mm", and a note
that dimensions in parentheses are on generated contents.

Writers: `to_svg()`, `to_pdf()` (our own PDF 1.4 writer: vector paths,
Helvetica text, no fonts embedded) and `to_dxf()` (AutoCAD R12 ASCII:
LINE, POLYLINE, SOLID, TEXT on one layer per style), and `write_sheet()`
by extension. All write fixed-precision numbers, so a drawing's files
are the same on every platform.

`standard_drawing(document, layout, name)` makes a starting drawing: front,
top, right and isometric views at the largest standard scale that fits
the sheet (1:1, 1:2, 1:5, 1:10 ..., 2:1, 5:1), with the layout's overall
width, depth and height as stable dimensions between the boundaries at
its extremes.

## CLI

`vstudio-cli drawing <project> [<name>] [--standard [name]] [--save]
[--svg out] [--pdf out] [--dxf out] [--config name] [--threads N]`:
evaluates the project, makes the drawing (or a standard one), prints
each view and each dimension with its value and `stable` or
`disposable`, the diagnostics, writes the files, and exits 1 on errors.
`--save` writes a `--standard` drawing into the project.

## App

`vs_app_core` (no Qt): `drawings.*` holds the commands (add a standard
drawing, rename, remove; add, change and remove views; add and remove
dimensions, move a dimension's line) and what the panel lists: drawings,
views, and the anchors a view offers for each part of each shown volume
and surface, marked stable or disposable. The history prunes drawings
after every edit.

The Qt part, on the Windows desktop: a Drawings dock (drawings, their
views and dimensions, with add / remove), a drawing window that renders
the `Sheet` with Qt and updates with every evaluation, a dimension dialog
(view, kind, two anchors from the offered lists, offset), Model > New
Drawing and File > Export Drawing… (SVG, PDF, DXF), which runs the
shipping check first like every export.

## Tests

- vs_model: JSON round trip, checks for every `model.drawing.*` code,
  links and configurations can't target a dimension, prune.
- vs_draw: view frames (each direction's axes, isometric's handedness),
  projection of a box (12 edges front, hidden edges of a box behind a
  box, a cylinder's silhouettes), meshes (creases and silhouettes,
  visibility), stable dimensions on the bracket equal its layout numbers,
  bolt hole diameters, disposable dimensions follow a regenerated
  parametric volume and a procedural one, a lost dimension warns, a stale
  volume warns, generated children are drawn and measured through their
  composite, the standard drawing, writers (SVG parses as XML, PDF's
  cross-reference offsets, DXF's sections), the same files on 1 and 4
  threads.
- CLI: `drawing --standard` on the bracket writes all three formats;
  a drawing on a procedural project lists disposable dimensions.
- App core: commands and anchor lists; app UI: the dock, the dialog,
  export (desktop).

## Left out

Section and detail views, angular and radial dimensions, tolerances and
GD&T, balloons and bills of materials, dimensions that drive the layout
from the drawing, multiple sheets per drawing, and fonts other than the
writers' defaults.

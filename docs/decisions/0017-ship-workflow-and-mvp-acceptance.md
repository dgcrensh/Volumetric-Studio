# 0017: Ship workflow and MVP acceptance

Nov 20, 2026 · M17

## Questions

1. STEP AP242 export "layout, boundaries and parametric volumes as B-rep;
   procedural volumes as mesh bodies or boundary only": what is a part, how
   are parts named, and how does a mesh go into STEP?
2. Import "STEP/STL into a volume from the app, with containment
   diagnostics": the volume's context menu already imported into an
   existing volume (M14). What was missing?
3. A "Validate" command "that runs every check and shows a summary before
   export": Model > Validate ran `validate::validate()` only, which can't
   see generated contents.
4. "Native project caches: optional saving, rebuild on load when missing or
   stale": what is stored, where, and what decides stale?
5. The end-to-end test: where does it live, and what does it drive?

## Decisions

**One assembly of named parts.** `kernel::write_step(parts, path, name)`
writes an XDE document: an assembly named after the design, one component
per part, each a product named after its volume (UTF-8 kept). The default
file has one part per volume, keep-outs excluded, in document order:
parametric and static B-rep contents as their solid, static mesh contents
and procedural contents as mesh bodies, placeholders as their boundary
(with `eval.export.placeholder`). The user's choice puts procedural
volumes in as their boundary instead, and two options add every volume's
boundary (`<name> boundary`) and every connection surface's patch (named
after the surface): that is "the layout" in the file, since the layout's
own geometry is exactly those boundaries and surfaces. Only current
results ship: a Failed, Stale or regenerating volume stops the export with
`eval.export.not_current`. `eval::step_parts()` / `export_step()`
(`step_export.hpp`) build the list; `kernel::read_step_parts()` reads it
back by name for tests.

**Mesh bodies are AP242 tessellated geometry.** `kernel::mesh_shape()`
makes a face with no surface carrying the triangles as its triangulation;
with `WriteTessellated = OnNoBRep` OCCT 8 writes it as a
`TRIANGULATED_SURFACE_SET` in a `TESSELLATED_SHAPE_REPRESENTATION`, every
triangle as generated (FP32 widened to FP64), and reads it back the same.
The alternative, one planar B-rep face per triangle, makes files tens of
times larger and slow to read for a gyroid's half a million triangles, and
looks to downstream tools like a solid that isn't one. Approximate
mesh-to-B-rep stays in L8.

**Import as a new volume from the File menu.** The engine already had
`import_volume()` (M9): File > Import as New Volume adds a STEP, IGES, STL,
3MF or OBJ file as a static volume boxed by its bounds, named after the
file, as one undoable `Command`. Import into an existing volume stays on
the volume's context menu; containment is checked by the engine as for any
contents and shows on the volume.

**Validate is every check against what is shown.** `app::check_for_export()`
(`ship.hpp`) is `validate::validate()` (rules, layout, overlap, contact,
connection types, B-rep containment), then every volume's diagnostics from
the evaluation on screen (generation, node failures, containment of
generated and imported meshes), a warning (`app.validate.stale`) for every
volume that isn't current, and the weld that STL and 3MF export would make
(minus `mesh.combine.empty`: a layout of placeholders can still ship as
STEP). A diagnostic both sources report is listed once. The diagnostics
panel shows the list; a one-paragraph summary goes to the status bar and a
dialog. Every export (STEP, STL, 3MF) validates first: errors stop it with
the summary, warnings ask "Export anyway?".

**Caches travel inside the project, by key.** The contents cache (M9)
already wrote `<key>.brep` + `<key>.json` to a folder; it now also writes
`<key>.mesh` (a small binary of positions, seam vertex ids, indices and
triangle volumes, bit for bit), so procedural meshes, the expensive part,
are cached too. A project's `cache/` holds those files in folders and now
in zips; `save_project(..., &cache)` replaces them and
`load_project_cache()` reads them (anything unreadable is simply absent).
They are never part of the content hash. `eval::project_cache_files()`
takes the shown evaluation's current contents, except parametric ones:
their feature rebuild (named faces for references and picking) lives in
memory only, and rebuilding is cheap next to generation. On open the
session loads the files into the engine's memory cache before the first
evaluation. Stale needs no rule: a key hashes every input, so an entry
whose inputs changed is never looked up, and the next save keeps only the
keys on screen. File > Save Caches With Project (on by default, kept
between runs) controls it; off, saving removes any caches the project had.

**The end-to-end test drives the libraries.** `vs_proc_tests`
"[mvp]" (`test_mvp_workflows.cpp`) builds a design from nothing through
the document API (`model::edit`, `eval::import_volume`, `proc::bake`, the
engine) and walks block out, fill, iterate, swap and ship, ending with a
saved zip whose caches let a fresh engine regenerate only the parametric
volume. It runs headless on Linux too, unlike the app. The app's own entry
points are covered by `vs_app_tests` "[ship]", and the GUI walk by
`docs/manual-tests/mvp-workflows.md`, which also maps every MVP bullet of
the spec to a test or a manual step.

## Gaps the end-to-end run uncovered

- Generated meshes were never written to the disk cache (B-rep only), so
  reopening a project always regenerated every procedural volume. Fixed by
  mesh entries.
- Model > Validate didn't see generated contents or the weld, so a design
  could validate cleanly and then fail to export. Fixed by
  `check_for_export()`.
- Cutting the welded mesh back into volumes (`mesh::extract_volume()`)
  gives pieces that are open where bonded seams were welded away. That is
  expected; the app's per-volume 3MF writes each volume's own closed mesh
  from before the weld, and the test checks the single welded object for
  watertightness.
- `kernel::bounding_box()` ignores mesh shapes (no surface); nothing needed
  it, and the header says so.

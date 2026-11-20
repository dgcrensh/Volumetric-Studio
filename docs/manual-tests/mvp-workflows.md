# Manual test: the MVP workflows in the app

Nov 20, 2026 · M17

This walks the spec's five workflows (block out, fill, iterate, swap,
ship) in `vs_app`, the way a user would. The same walk runs headless through
the document API in `vs_proc_tests "[mvp]"` (`libs/vs_proc/tests/test_mvp_workflows.cpp`),
and the app's own entry points are tested in `vs_app_tests "[ship]"`. The
table at the end maps every MVP bullet of the spec to the tests and steps
that cover it.

Build with the `windows-msvc-app` preset, then start the app with no
project: `build/windows-msvc-app/apps/vs_app/vs_app.exe`. Tick each step
when it behaves as described. Times are rough, on a 4-core machine.

## 1. Block out

1. [ ] File > New. The volume tree is empty; the title says Untitled.
2. [ ] In the Variables panel, add `w` = `40 mm`.
3. [ ] Insert > Datum Point twice: `Body min` at (0, 0, 0) and `Body max` at
   (`w`, 30, 20). Then `Infill min` at (`w`, 0, 0) and `Infill max` at
   (`w + 40 mm`, 30, 20).
4. [ ] Volume > New Volume: `Body`, a box between `Body min` and `Body max`.
   Again for `Infill`. Both appear in the tree as placeholders, Current.
5. [ ] Insert > Datum Plane `Joint`: offset from YZ by `w`.
6. [ ] Volume > New Connection Surface: `Bond`, bonded, on `Joint`, 10 x 10
   centred at (15, 10), joining Body and Infill. The viewport highlights it.
7. [ ] Model > Validate (Ctrl+R): the dialog says "Ready to export"; the
   Diagnostics panel is empty or has infos only.
8. [ ] Move `Infill min` to (`w - 5 mm`, 0, 0): Validate now reports
   `validate.overlap` naming both volumes, and Export STEP refuses with the
   summary. Undo (Ctrl+Z); Validate passes again.

## 2. Fill

1. [ ] Right-click Body > Fill by modelling, then Insert > Sketch on its top
   face (`+z`): a 40 x 30 rectangle from the corner. Insert > Extrude it
   down 20 mm. Done editing contents. Body turns from placeholder to
   modelled, Current.
2. [ ] Right-click Infill > Attach Template > `gyroid_infill`, cell size 1.
   The viewport shows the infill after a few seconds; the progress bar runs
   meanwhile.
3. [ ] File > Import as New Volume (Ctrl+I): a STEP file of a small box away
   from the others (any `kernel-demo.step` from `vstudio-cli kernel-demo`
   works). It appears as a static volume named after the file, boxed by its
   bounds.
4. [ ] File > Import as New Volume: an STL (`libs/vs_mesh/tests/data/hex_prism.stl`).
   A second static volume appears.
5. [ ] Right-click the STL volume > Import contents… and pick a STEP file
   bigger than it: the volume turns Failed with `validate.containment`
   naming it, and keeps showing its last contents greyed. Undo.

## 3. Iterate

1. [ ] Select the Body min/max extrude or the `w` variable and drag its
   handle in the viewport (or change `w` to `45 mm`). During the drag Body
   and Infill show their boundaries only; the imported volumes keep their
   contents. The viewport stays smooth.
2. [ ] On release, Body rebuilds at once and Infill regenerates in the
   background, greyed until done. The imports don't regenerate: select
   one and its Cache key in the inspector is what it was before.
3. [ ] Press Esc during Infill's regeneration: it stops, Infill shows its
   last contents greyed (Stale). F5 regenerates it.

## 4. Swap

1. [ ] Right-click Infill > Swap contents > Baked. It becomes static
   (baked), same shape. Nothing else regenerates.
2. [ ] Swap contents > Procedural (Unbake): the graph comes back, and
   Infill's Cache key in the inspector is the one it had before baking.
3. [ ] Swap Body to Placeholder and back with Undo: Infill and the imports
   don't regenerate.
4. [ ] Right-click Infill > Fillet…:
   an inline bar explains that generated contents are edited through their
   procedure, with an "Edit graph" button that opens the node editor.

## 5. Ship

1. [ ] Model > Validate: "Ready to export", with any warnings listed.
2. [ ] File > Export STEP…: keep "Mesh bodies", tick both "Also include"
   boxes, save. The status bar reports the parts (B-rep, mesh, boundary).
   Open the file in another CAD tool or viewer (FreeCAD, an online STEP
   viewer): one assembly named after the design, a part per volume by name,
   the infill as a triangulated body, plus the boundaries and `Bond`.
3. [ ] Export STEP again with "Their boundary only": Infill comes in as its
   box.
4. [ ] File > Export 3MF… and Export STL…: both succeed. Open the 3MF in a
   slicer: one object per volume, named, no repair warnings. The STL is one
   watertight mesh.
5. [ ] Leave Infill regenerating (change `w`, then export at once): every
   export refuses with "Wait for the evaluation to finish" or, after it
   stops early, Validate's warning that Infill isn't current.
6. [ ] File > Save As `walk.vsproj` with File > Save Caches With Project
   ticked. Close and reopen it (File > Open Recent): the infill shows at
   once, without regenerating (no progress bar); Body rebuilds in well under
   a second.
7. [ ] Untick Save Caches With Project, Save, reopen: Infill regenerates.

## 6. Print a real part

1. [ ] Design a real part (for example the bracket from
   `libs/vs_model/tests/data/bracket.vsproj` with a `gyroid_infill` in the
   Base), export the 3MF, slice it and print it. It prints without slicer
   repairs and the infill is where the design put it.

## The MVP, bullet by bullet

| Spec MVP bullet | Covered by |
| --- | --- |
| Parametric sketches and features, enough to author boundaries and connection surfaces | vs_sketch and vs_feature suites; `vs_app_tests` "The bracket can be rebuilt from scratch entirely in the app"; "[mvp]" block out; manual 1.1–1.6 |
| Static, parametric and procedural volumes, with one level of nesting | `vs_model_tests` (bracket, encapsulation); `vs_eval_tests` "[import]"; "[mvp]" fill; manual 2.1–2.4 |
| Connection surfaces with geometry, seam definition and 2–3 types (bonded, clearance, bolted flange) | `vs_model_tests` build; `vs_validate_tests` "Connection type rules"; `vs_mesh_tests` "The bracket's bolted flange gets a seam with its bolt holes", "Clearance surfaces get no seam; one-sided ones do", weld suites; "[mvp]" bonded weld; manual 1.6 |
| Node graph with field and mesh types; lattice, TPMS, shell, scatter and instancing nodes | `vs_proc_tests` "Strut lattices…", "TPMS sheets…", "Scatter in the volume…", "Instances place a template…", "Mesh instances…", "Mesh booleans go through Manifold"; `vs_app_tests` "The M12 templates built from scratch in the editor match their saved versions"; manual 2.2 |
| All seven design rules enforced, with the diagnostics listed above | 1 encapsulation: `vs_model_tests` "The layout can't refer into a volume's contents", "Contents see only their own boundary and connection surfaces". 2 full authority and 3 fixed inputs: `vs_proc_tests` "A graph may read only the surfaces joining its volume", `vs_model_tests` "Contents see only…". 4 no overlap: `vs_validate_tests` "Penetration up to the tolerance is touching, beyond it overlap". 5 containment: `vs_validate_tests` "B-rep contents inside, flush with and outside their boundary", "Mesh contents"; `vs_app_tests` "Validate sums up every check before export". 6 one-way flow: `vs_model_tests` "An output feeding something upstream of its volume is a loop". 7 determinism: `vs_eval_tests` "Output hashes are identical with 1, 4 and 16 threads". Diagnostics: `vs_validate_tests` "Diagnostics carry what the spec asks them to name", "Every diagnostic routes to exactly one place"; manual 1.8, 2.5 |
| Dependency-graph scheduler, caching, background regeneration with previews | `vs_eval_tests` "Changing a parameter regenerates only that volume and its dependants", "The preview tier shows boundaries at once", "Project caches let a new engine skip what still matches"; `vs_app_tests` "Dragging a layout dimension on the bracket keeps the viewport above 30 fps with previews"; "[mvp]" iterate and reopen; manual 3, 5.6 |
| 3MF, STL and STEP export (procedural volumes as mesh bodies); STEP and STL import | `vs_eval_tests` "[step]", "[import]"; `vs_kernel_tests` "STEP parts keep their names, B-rep solids and mesh triangles"; `vs_app_tests` "Every sample project that evaluates cleanly exports STL and 3MF", "The app validates, imports, exports STEP and saves caches"; "[mvp]" ship; manual 2.3–2.4, 5.2–5.4, 6 |

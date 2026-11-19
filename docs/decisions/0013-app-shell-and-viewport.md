# 0013: App shell and viewport

Nov 19, 2026 · M13

## Questions

1. The plan puts OCCT's AIS viewer in the app, but OCCT types never leave
   `vs_kernel`, so the app can't hand a `TopoDS_Shape` to `AIS_Shape`.
   How does the viewport draw B-reps?
2. Where does the engine run, and how do results reach the UI thread
   without a frame over 100 ms?
3. How is the app tested, when the app isn't a library and CI doesn't
   build it?
4. What happens to a project that breaks the authoring rules (a
   dependency loop), which `load_project()` refuses by default?

## Decisions

**Everything in the viewport is a triangle mesh.** B-reps (boundaries,
parametric contents, imported STEP files, connection surface patches) are
tessellated with `kernel::tessellate()` at display resolution (0.05 mm
deflection, 0.35 rad), and their B-rep edges come along as polylines.
Meshes (procedural contents, imported STL/3MF/OBJ) are drawn as they are.
The app links only OCCT's visualisation toolkits (`TKService`, `TKV3d`,
`TKOpenGl`); the modelling toolkits stay private to `vs_kernel`, and no
app header includes OCCT. One custom `AIS_InteractiveObject` draws one
item from prepared `Graphic3d` arrays. Normals are per vertex, with
vertices split where faces meet at more than 0.6 rad, so flat faces stay
flat and curved ones smooth.

What is drawn: each volume's boundary as a transparent tint of the
volume's colour with its edges (more opaque when the volume is still a
placeholder), its contents shaded (grey when shown stale), keep-outs in
red, and connection surfaces coloured by type (bonded green, clearance
blue, bolted flange orange), offset towards the camera so they win over
the boundary faces they lie on. Transparency is order-independent
(`Graphic3d_RTM_BLEND_OIT`), with 4x MSAA.

Picking: boundaries and connection surfaces are pickable; contents are
not, and are picked through their boundary, which encloses them. That
keeps OCCT from building a selection BVH over a million-triangle
procedural mesh on the UI thread.

**One worker thread, two snapshots per evaluation.** `Session` owns the
project, an `eval::Engine` with `proc::install()`, and one worker thread.
`regenerate()` cancels the running evaluation and queues a new one; the
worker runs only the newest request. Each request is delivered twice, as
an immutable `Snapshot`: first `Engine::preview()` (boundaries, surfaces
and cached contents, at once), then the full evaluation. The worker also
builds the scene (tessellation, normals) and the viewport's `Graphic3d`
arrays, so the UI thread only swaps display objects. While an evaluation
runs, a 100 ms timer reads `Engine::states()` for the Regenerating badges.
Export and Validate run on the same worker, in order.

**The app is a library plus `main()`.** Everything but `main.cpp` is
`vs_app_lib`; `vs_app_tests` (Catch2 with a `QApplication`) drives the same
code. Headless tests cover the scene, panels, routing, export and the
session; tests tagged `[gui]` open the main window, render with OpenGL and
need a desktop session with a GPU. Both targets register under the
`vs_app` module, so the dependency rule treats them as the application.
CI still builds only the libraries; `windows-msvc-app` is run on Dallas's
desktop.

**The app opens what the validator opens.** Files are loaded unchecked
(`load_project(path, false)`), so a document that breaks the authoring
rules still opens, shows nothing in the viewport (the engine builds
nothing) and lists why in the diagnostics panel. Clicking a diagnostic
selects its `validate::route()` target: a volume, a connection surface,
a feature (in the layout or a volume's tree, found through the sketch
that holds an entity or constraint), or a graph node, whose volume the
viewport highlights.

**No text on the view cube.** vcpkg's OCCT is built without FreeType
(`default-features: false`), so the cube's faces and axes have no labels;
its coloured axes show the orientation. Turning FreeType on would rebuild
OCCT (over an hour) for a cosmetic gain.

**The engine leaves two hardware threads to the UI.** With every core
generating, the UI thread and the GPU driver were starved and frames
stalled for 110–140 ms early in a long regeneration. The session's engine
pool therefore has `hardware_concurrency() - 2` threads (12 → 10 here).

**Big meshes reach the GPU a part per frame.** A display mesh over 60,000
triangles is split into parts (each with only its vertices) when it is
prepared; the first part of every item shows at once and the rest one per
16 ms tick, so no frame uploads a million triangles.

**Every paint redraws the whole view.** Displaying or highlighting objects
doesn't invalidate the V3d view, so without a full redraw the window kept
showing the preview frame (the off-screen render was right; a test now
compares the two).

**Qt from vcpkg, built lean.** vcpkg only lets the top-level manifest turn
off a dependency's default features, so the `app` feature also lists
host `qtbase` with `default-features: false`; otherwise the host tools'
Qt pulls ICU, OpenSSL, PostgreSQL and more (and ICU's autotools build
fails on the space in "Volumetric Studio"). The target Qt has no PNG
support, so `vs_app --screenshot` is given `.bmp` paths. Qt also refuses
to configure under a path it thinks contains a symlink, which is how the
Claude desktop app's virtualised `%LOCALAPPDATA%` looks: builds from
there pass `VCPKG_INSTALL_OPTIONS=--x-buildtrees-root=C:/vsbt`.

## Results

On Dallas's desktop (12 hardware threads, `windows-msvc-app`):

- All 462 tests pass, 46 of them `vs_app` tests (5 `[gui]`).
- Every sample project (`bracket`, `bolt_bosses`, `shell_and_ball`, the six
  validation fixtures) and an imported STL open, draw their boundaries,
  contents and connection surfaces where the kernel says they are, and
  the ones that evaluate cleanly export STL and 3MF that read back
  (watertight). The loop fixture opens with its error and draws nothing;
  the placeholders-only fixture refuses export with `mesh.combine.empty`.
- Responsiveness: `shell_and_ball` sampled at 0.15 mm regenerates in about
  3.8 s into 862,240 triangles (display data 110 ms on the worker). Over
  four runs the longest gap between 10 ms UI timer ticks was 13–39 ms and
  the longest frame 8–37 ms, against the 100 ms budget. A fresh app's
  first few frames take 80–140 ms (OpenGL setup and shader compilation);
  the test warms the viewer up with a small project first.

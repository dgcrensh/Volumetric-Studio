# 0016: Interactive regeneration and guidance

Nov 20, 2026 · M16

## Questions

1. Dimension dragging "in the viewport and sketcher with M2 drag solving":
   which numbers get handles, and what does dragging one change, when most
   of the bracket's dimensions are driven by variables (`width`, `base_h`)?
2. "Boundary-only or low-detail previews during the drag": how, without the
   preview waiting behind a generation, and without a frame over 33 ms?
3. "Stale results drawn greyed out until replacements arrive; a progress
   indicator and cancel for long generations." Contents already turned grey
   (M13); but a procedural job, once started, ran to the end (decision
   0010), so Cancel did nothing to a single long volume.
4. Swapping contents "with nothing outside the volume touched (verified by
   hash)": what is swapped to what?
5. Habit guidance: where does the user meet it, and what is "the right
   action"?
6. Face and edge picking (deferred from M14, decision 0014): how does a
   click name a face as the lists name it, and how does edit in context
   stay enforced?

## Decisions

**Handles drag what drives the number.** A selected sketch shows a handle
per dimension (distances, horizontal and vertical distances, radii,
diameters; not angles), a selected extrude its depth (blind or symmetric)
and its sketch's dimensions. A handle sits on what it sizes (a line's
middle, a circle's rim, the middle of the extrude's far face) and moves
along the way the value grows. If the number is one variable's name, the
drag sets the variable (`base_h = 18 mm`), so everything that uses it
follows, as the design intends; a plain number is set itself, the sketch
solved again from where it is (M2's solver moves as little as it can); a
longer expression (`width / 2`) gets no handle, as there's no one right
number to change. The same rules hold in sketch mode, where dragging a
dimension's label changes it (`SketchEditor::drag_dimension()`); there a
dimension an expression drives says so, since the variables are outside
the sketch. `dimension_drag.hpp` has the rules without Qt.

The viewport draws handles as small diamonds of a fixed size on screen, on
top of everything, and hit-tests them itself by their projected anchors, so
a press on one drags it instead of orbiting. The drag distance is the mouse
movement projected on the handle's direction on screen.

**A drag shows the preview tier only.** `Session::drag_preview()` runs
`Engine::preview()` (layout, boundaries, validation, cached contents; no
generation) on the worker for the edit applied to a copy, and the scene
leaves out contents shown stale, so a volume the drag changes shows its
boundary and untouched volumes keep their contents. Releasing the handle
applies the same edit through the History (one undo step) and regenerates
in full. A drag asks for a preview every frame, faster than previews come:
the worker runs only the newest, and each preview is shown when it arrives
unless something newer was shown or a regeneration was asked for since (the
M13 rule "only the newest request reaches the screen" would have shown
none until the mouse stopped). The UI thread only swaps display objects, so
frames stay short: dragging the bracket's base height for 3 s on the VM
drew 181 frames (60 fps, the timer's rate), the longest 15 ms, with 82
previews.

**Procedural jobs stop when cancelled.** The procedural generator now
passes the evaluation's cancellation token to the graph evaluation and the
sampling and checks it before and after meshing; a job that stops sets
`GeneratedContents::cancelled`, and the engine treats it as a volume it
never started: Stale, its last contents kept (grey), nothing cached, so the
next evaluation generates it. Decision 0010's "a job runs to the end" was
right for kernel operations, which can't be interrupted, but a procedural
volume is the long one, and Cancel must reach it. The status bar shows a
progress bar (volumes done of total) and a Cancel button while an
evaluation runs.

**Swapping is between four kinds, one step each.** The volume's context
menu offers placeholder, modelled (empty parametric contents), procedural
(the starter graph, or a baked volume's own graph back) and baked (from the
procedure's current mesh). Each is one Command that changes only that
volume's contents; imported static contents can be swapped away from but
not to. Cache keys hash only the volume's own definition, boundary,
surfaces, parameters, seed and the outputs it reads, so other volumes' keys
can't change unless they read this volume's outputs through a link; the
tests check every other volume's key, and the document with this volume's
contents blanked, after every swap (and the bracket's base, which feeds a
link, is left alone).

**Guidance is an inline bar with one button.** Insert > Fillet or Chamfer
with a volume of generated contents selected (procedural, baked or
imported), and the volume's context menu's Fillet, Cut and Mate, show a
yellow bar under the toolbar: a title, two or three sentences, and a button
that does the right thing (`guidance.hpp`): fillet a procedure by editing
the procedure (unbake a baked one first; modelled contents are filleted in
context, where the bar opens the fillet dialog); cut by adding a keep-out
region; mate by selecting the connection surface that joins the volume (or
making one). Nothing is blocked: the bar can be dismissed.

**Faces are parts of B-rep items; a pick is named by surface.** The scene
now keeps, for every B-rep item, each face's triangle range (the faces are
tessellated one after another) with its kernel face, and each drawn edge's
two faces. In pick mode the viewport activates a second selection mode, one
sensitive triangle set per face, or segments per edge, with an owner that
carries the face or edge index. A picked face is named by matching its
surface (plane and outward normal, or cylinder) with the surfaces the
offered references lie on, in list order: as a FaceRef resolves by geometry
(decision 0002), two references on one surface facing one way name the
same faces of the body, and the first in the list is the one a pick gives.
A patch names itself (as a plane, the surface). An edge is the pair of its
faces, each named the same way: the EdgeRef fillets and chamfers take.

Only items edit in context allows are pickable: in the layout, boundaries
and layout solids; in a volume, its boundary, its B-rep contents and the
connection surfaces joining it. Generated and imported meshes have no faces
and stay unpickable, as before. Clicking again at the same place picks what
lies behind the last pick there, so a face hidden by another (the upright's
underside, on the base's top) can still be clicked. Every plane and face
field of the feature dialogs, a shell's faces and a fillet's or chamfer's
edges have a Pick button; the lists stay as the fallback.

Planes that aren't faces (the principal planes, datum planes, sketch
planes) and axes and points aren't drawn, so they stay list-only.

## Results

On Dallas's GCP VM (4 vCPUs, L4, `windows-msvc-app`):

- Dragging the bracket's base height (`base_h`, 10 to 18 mm) for 3 s: 60
  fps, longest frame 15 ms, 82 boundary-only previews; one undo step,
  `base_h = 18 mm`, the base 18 mm tall after the full generation.
- Every face reference a datum plane or fillet dialog offers on the
  bracket, in the layout and in the base's context, is picked by clicking
  a face on it, and resolves to the first list choice on that surface;
  every edge between two named faces is picked and lands in the fillet's
  edge list as that pair.
- Swapping the upright through placeholder, procedural, baked, procedural
  and modelled leaves the base's cache key and everything outside the
  upright unchanged; undoing every step restores the original hash.
- Cancelling a 0.3 mm shell-and-ball regeneration stops it part way; its
  old contents stay, grey, and the next evaluation generates it.

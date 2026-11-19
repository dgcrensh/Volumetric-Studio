# 0014: Sketcher and feature editing UI

Nov 19, 2026 · M14

## Questions

1. Undo and redo must restore identical document hashes (M14 acceptance).
   What does a command store?
2. The plan says "sketch mode: draw on a plane". Where is the sketch
   drawn: in the 3D viewport or somewhere else?
3. Feature dialogs need references (planes, axes, points, faces, edges).
   How does the user pick them, and how does "edit in context" make sure
   only the volume's boundary and connection surfaces can be chosen?
4. How does a dialog preview its change live without polluting the undo
   history?
5. Which edits does the app refuse, and which does it accept and show as
   failed?
6. While blocking out, a solid feature may not belong to any volume yet,
   and sketches have no volume at all. How are they shown?

## Decisions

**A command is a function; a step stores whole snapshots.** Every edit is
a `Command` (`authoring.hpp`): a label for the Edit menu and a function
that changes a `model::Project` in place. `History` (`history.hpp`) runs
it on a copy, checks the copy with `model::check()` and keeps it only if
the change added no error. A step holds the project before and after as
shared immutable snapshots, so undo hands back the very project that was
there: ids, order and blobs included, hence the same `content_hash()`.
Inverse operations were rejected: every one would need its own exact
inverse (variable renames, feature moves, volume removal that drops links
and surfaces), each a place for a hash to drift. Snapshots cost a document
copy per step, which is small next to an evaluation (the bracket is a few
kilobytes; blobs are copied once per step that touches them).

A document opened with errors (the app opens what the validator opens)
stays editable: a change is refused only when it adds an error that wasn't
there, so errors can be fixed one at a time.

**Sketch mode is a 2D canvas that replaces the viewport.** `SketchMode`
(a toolbar, `SketchView`, the constraint list and the solver's status)
takes the central widget's place until Finish or Cancel. A sketch is 2D
in its plane's frame, so a flat canvas shows it exactly, with no camera to
align, no picking through OCCT and pixel-sized snapping. The rules live in
`SketchEditor` (no Qt): tools (point, chained lines, rectangle, circle,
arc, slot), snapping by shared points, constraints that fit a selection,
dimensions with expressions over the layout's variables (or the volume's
parameters), drag solving, and its own undo stack for the session.
Finishing hands the sketch to the History as one step. A conflicting
constraint is kept and drawn red, and the status names the conflicting
constraints ("Conflicting constraints: Distance 3, Distance 1"); the
solver leaves the geometry where it was. Lines within 2 degrees of an
axis get horizontal or vertical constraints automatically; rectangles get
both; slots are two lines tangent to two arcs of equal radius.

**References are chosen from lists computed for the tree being edited.**
`reference_choices()` lists what a feature at a given position may refer
to, labelled for people ("Base block end", "Upright outline: Line 3",
"Boundary: Base block end", "Surface: Flange"). In the layout that is the
principal planes and axes, every feature above and the faces solid
features name. Inside a volume it is only the volume's inputs (its
boundary's named faces, the connection surfaces that join it) and the
contents' own features: no principal planes, nothing of the layout,
nothing of another volume. So encapsulation holds by construction, and a
test resolves every offered reference with `model::resolve()` from inside
the volume. Edges are chosen as M4 names them: the edges a face shares
with another face, or every edge of a face. Picking faces in the 3D view
was left out of M14 to keep the milestone's scope, not because it is
blocked: today each B-rep item is one triangle array, so a click names
the item (the volume), not the face. Picking needs the per-face triangle
ranges the kernel's tessellation already has, kept as pickable parts, and
a match from the picked face to the named face whose plane or cylinder it
lies on. It is an explicit M16 goal; the lists stay as the fallback.
A reference a list doesn't offer (a broken one) is kept and shown as
"(missing reference)", so opening and closing a dialog never changes what
a feature refers to.

**Previews are edits on a copy, shown but not kept.** Dialogs are
modeless. Each change (debounced 150 ms) builds the dialog's command,
`History::try_apply()` makes the changed project without keeping it, and
the session evaluates that; the status bar says "preview". OK applies the
same command through the History (one undo step); Cancel shows the
History's project again. The engine's cache makes previews cheap: only
what the change touches regenerates.

**Model errors are refused; rebuild failures are accepted.** An edit that
breaks the document's authoring rules (a duplicate name, a reference into
another volume's contents, a variable loop, a keep-out with contents) is
refused, with the reason in the dialog and the diagnostics list. An edit
that only makes a feature fail when rebuilt (an unknown variable in a
depth, a fillet too large) is kept, as in any CAD tool: the feature shows
"failed" and the diagnostics say why and where to act.

**Edit in context dims and locks the rest.** Volume > Edit contents (or
"Fill by modelling" on a placeholder, which first makes it parametric)
makes that volume's tree the one Insert adds to and the feature and
variables panels show. The viewport draws every other item grey and
nearly transparent and makes it unpickable; the volume's own boundary,
contents and sketches, and its connection surfaces, stay as they are.

**Layout solids and sketches are drawn too.** A layout solid feature that
no boundary or keep-out uses yet is drawn as a grey tint (pickable: it
selects the feature, and New volume then starts from it). Sketches are
drawn as lines on their planes; a sketch a feature or connection surface
already uses is "secondary" and shows only while selected, so finished
models aren't covered in sketch lines.

## Results

On Dallas's desktop (`windows-msvc-app`):

- The bracket rebuilt from scratch in the app (variables through the
  variables table, sketches drawn and dimensioned in sketch mode, features,
  volumes, the bolted flange, links and the base's contents through their
  dialogs, the upright's STEP file imported) takes 29 undoable steps,
  builds and validates cleanly, and matches the sample: the same layout
  features and variables, boundaries, contents, outputs (mass, volume,
  envelope), flange area and hole centres. Undoing all 29 steps restores
  every intermediate document hash and the empty project's; redoing them
  restores each again.
- Every dialog reads back exactly the feature or model value it was given
  (the tests round-trip each one through its widgets before committing).

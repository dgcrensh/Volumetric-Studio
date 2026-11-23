# U1 — Context toolbar (plan)

Nov 23, 2026 · follows L6 (drawings) · decision record:
`docs/decisions/0039-context-toolbar.md`

Status: phases 1 to 4 done; phases 5 and 6 to do.

## Goal

The app's tools are buried in the Insert, Volume and Model menus and in
right-click menus, and the Diagnostics dock takes the bottom of the window
and can't be folded away. U1 makes tools follow what you are doing and
keeps the viewport the centre of the window (concept A of the three tool
flow concepts Dallas reviewed):

- A tab row (Layout, Contents, Sketch, Graph, Drawing) above one labelled
  toolbar whose tools change with the tab. Tabs that can't be entered from
  the current selection are dimmed and say why.
- The Insert, Volume and Model menus go; their actions move to the
  toolbar, and right-click menus stay. File, Edit, View and Help stay.
- Sketch mode's toolbars move into the Sketch tab, its constraint list to
  the right panel, and the browser folds to a strip while sketching.
- Volumes, Features, Variables and Drawings become one foldable Browser
  dock; the Inspector folds to a strip.
- The Diagnostics dock goes: an error and warning count in the status bar
  opens a Problems drawer under the viewport, closed by default.

Only `apps/vs_app` changes; no library, file format or hash changes.

## Where the app is

The window is a `QMainWindow` whose central widget is a stack of the OCCT
viewport and, while sketching, `SketchMode`. Two facts shape the plan:

- The viewport is a native window (`WA_NativeWindow`, OCCT draws into its
  HWND), so Qt widgets can't be drawn over it on Windows. The Problems
  drawer and the view buttons sit beside or below it.
- `test_dialogs_ui.cpp` finds actions by text and counts the holders of
  Undo, Redo, Esc and F. Toolbars reuse the menus' `QAction`s, never copies.

## Phases

1. **Tool catalogue and icons** (Qt-free, `vs_app_core`). `tool_catalog.*`:
   `Context`, the catalogue of tools (id, name, label, tooltip, shortcut,
   icon), each context's groups, `ToolState`, `current_context()`,
   `tab_availability()` and `availability()` with reasons. `icons.*`: every
   icon as stroke geometry on a 24-unit grid. Tests:
   `test_tool_catalog.cpp`. **Done.**
2. **Shared actions and the context bar.** `actions.*` makes one
   `QAction` per tool, connected to the `MainWindow` entry points that exist
   (`new_volume()`, `insert_feature()`, `edit_graph()`, `validate()`, ...);
   `icon_painter.*` paints catalogue icons; `context_bar.*` is the tab row
   and one toolbar per context (text under icon, group captions, a "More"
   menu past `ToolGroup::shown`). `build_menus()` keeps File, Edit, View
   and Help; the "Editing in context" bar goes; the Configuration box and
   Regenerate move to the top row. **Done.** The Design Table moved there
   too; Cancel (Esc) stays a window action behind the status bar's
   button. Sketch tools already drive `SketchMode` here; its own
   toolbars go in phase 3.
3. **Sketch tab.** `SketchMode` loses its toolbars and buttons; the Sketch
   tab's actions call `set_tool()`, `constrain()`, `toggle_construction()`,
   `remove_selection()` and `finish()`. Constraint tools are enabled by
   `SketchEditor::applicable()` on the sketch selection. The constraint
   list and solve status move to the right panel; the Browser folds.
   Also from phase 2: the Sketch toolbar fits from about 1440 px wide; in
   narrower windows Qt still elides its longest labels, so this phase
   makes it fit the window's 1284 px minimum (shorter labels or icon-only
   constraint buttons), and sketch mode stops growing the window 55 px.
   **Done.** The panel (status, constraint list, hint) is
   `SketchMode::side_panel()`, shown in a Sketch dock tabbed with the
   Inspector; the left docks fold until Finish or Cancel (phase 4's
   Browser will fold to a strip instead). The drawing tools are checkable
   and follow `SketchMode::toolChanged()`. The Sketch toolbar fits by
   showing five drawing tools, four constraints and two dimensions with
   the rest under More, and "H distance" and "V distance" labels; and in
   any window too narrow for a toolbar's labels (the narrowest is about
   1050 px, set by the docks) its buttons show icons only, labels in
   their tooltips, so no toolbar widens the window or elides a label.
4. **Browser and Inspector.** `browser.*`: one left dock with foldable
   sections holding the existing trees, so `volume_tree()`,
   `feature_tree()` and `variables_table()` keep working. The Inspector
   folds to a strip. **Done.** `FoldPanel` is a side panel with a header
   (title and fold chevron) that folds to a 36 px strip of buttons (unfold,
   then one per section, which unfolds on that section); the window
   resizes the dock to the strip and back (`fold_dock_with_panel()`).
   `Browser` holds Volumes, Features, Variables and Drawings (closed until
   a drawing opens) as sections whose headers fold them to a line; open
   sections share the height. The Volumes, Features and Variables docks
   are gone; the Browser sits in the dock that held Drawings, so
   `drawings_dock()` and its View menu toggle keep working, and the
   Inspector is a `FoldPanel` on the right. Sketch mode folds the Browser
   to its strip and puts it back as it was. Tests: `test_browser_ui.cpp`.
5. **Problems.** `problems.*`: the status-bar count and the drawer, a pane
   under the viewport in a vertical splitter, holding the existing
   diagnostics tree (`diagnostics_list()`, `activate_diagnostic()` stay).
   It opens itself only after something you started fails (Validate, a
   refused edit, an export check), never for a background evaluation.
   Rows in the Browser show their counts. The Graph dock shows in the
   Graph context only.
6. **Drawing tab and finish.** `SheetView` moves into the central stack
   under the Drawing tab (the separate drawing window goes); one
   stylesheet with the mockup's colours; `QSettings` for panel widths,
   folded sections and the drawer height; `docs/manual-tests/
   mvp-workflows.md`, `CLAUDE.md`, this plan and the status table.

## Tests

| Acceptance item | Test | Suite |
| --- | --- | --- |
| Each window state maps to one context; dimmed tabs give a reason | `current_context()`, `tab_availability()` | `vs_app_core_tests` |
| Every tool has a unique name, a label, a tooltip and an icon; shortcuts are unique and leave the window's keys free | catalogue walk | `vs_app_core_tests` |
| Tools are enabled only where they work (bake a procedure, unbake a baked volume, nothing on generated children, constraints that fit the sketch selection) | `availability()` on the bracket and nested samples | `vs_app_core_tests` |
| Each toolbar action reaches the entry point its menu reached | trigger each action | `vs_app_tests` |
| Edit in context selects Contents; opening a sketch selects Sketch; Finish and Cancel go back | drive `edit_in_context()`, `edit_sketch()`, `finish_sketch()` | `vs_app_tests` |
| One holder per Undo, Redo, Esc and F outside sketch mode | existing check | `vs_app_tests` |
| The drawer starts closed, the count matches, Validate with an error opens it, a background evaluation doesn't | `test_window.cpp` | `vs_app_tests` |
| Folding panels widens the viewport; the layout comes back after a restart | geometry, `QSettings` round trip | `vs_app_tests` |
| The window looks like the mockup | `[gui]` screenshots, by eye | desktop |

## Out of scope

A dark theme; problem pins in the viewport (OCCT labels in its topmost
layer: a good next step); Ctrl+K search over the catalogue; sketching on
the plane in 3D; dialog changes.

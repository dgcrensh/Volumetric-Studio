# 0039: Context toolbar

Nov 23, 2026 · U1 · plan: `docs/plans/U1-context-toolbar.md`

## Questions

1. How does the window know which tools to show?
2. Where do tools live: menus, toolbar, or both?
3. Where do icons come from?
4. How do diagnostics stay visible without taking the viewport's room?
5. What happens to drawings' separate window?

## Decisions

**The context is derived, never stored.** `current_context()`
(`tool_catalog.hpp`) reads what the window has open: sketch mode, then a
shown drawing, then an open graph, then contents edited in context, else
the layout (a composite edited in context is a layout too). The tabs show
it and can't disagree with the window. Clicking another tab enters it the
way the old menu item did; `tab_availability()` says when it can't and
why, and while a sketch is open every other tab waits for Finish or
Cancel.

**One catalogue, one QAction per tool.** Every tool is a row of a Qt-free
catalogue (stable name, label, tooltip, portable shortcut, icon) and each
context's toolbar is named groups of rows. `availability()` says whether a
tool can act on the selection now, with a sentence for the tooltip when it
can't (a fill tool on a generated volume, a constraint that doesn't fit
the sketch selection). The rules are in `vs_app_core`, so every CI job
tests them; the window makes one `QAction` per row and shares it between
the toolbar, the remaining menus and right-click menus, so a shortcut
never has two holders (H7) and tests that find actions by text still find
them. The Insert, Volume and Model menus go: the toolbar and the
right-click menus cover them, and two copies would drift. The shortcuts
the menus had carry over, except Insert > Sketch's Ctrl+Shift+S, which
File > Save As (`QKeySequence::SaveAs`) also held, so neither worked: the
Sketch tool takes Ctrl+Shift+K. Toolbar buttons never shrink (Qt would
elide their labels); a context with too many tools shows fewer and puts
the rest under More, and a window too narrow for the labels shows the
buttons' icons only, with the label in the tooltip.

**Icons are stroke geometry.** `icons.hpp` gives each icon a few strokes
on a 24-unit grid (polylines with circles and arcs flattened, dots,
dashes), painted with a round pen in the text colour. No image files, no
QtSvg (a new vcpkg dependency would need a desktop build and a Windows
cache republish, decision 0005), and crisp at any size and in any theme.

**Problems sit under the viewport, not over it.** The viewport is a native
window that OCCT draws into, and Qt widgets can't be painted over it on
Windows. So the Diagnostics dock becomes a status-bar count and a drawer
in a vertical splitter under the viewport, closed by default. It opens
itself only after something the user started fails (Validate, a refused
edit, an export check); background evaluations update the count only.
The view buttons are a slim toolbar beside the viewport, with OCCT's view
cube inside it as now.

**Drawings move into the main window.** Under the Drawing tab the sheet
takes the viewport's place in the central stack, as sketch mode does; the
separate drawing window goes.

## Consequences

- Phase 1 (catalogue and icons) builds and tests on Linux; the window
  phases need the app preset on the desktop.
- New tools are added as catalogue rows, with their availability rule and
  a group; the catalogue test checks names, shortcuts and icons.
- Problem pins in the viewport, when they come, are OCCT labels in its
  topmost layer, which a native window allows.

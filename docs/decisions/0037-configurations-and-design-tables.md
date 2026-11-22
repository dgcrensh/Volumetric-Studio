# 0037: Configurations and design tables

Nov 22, 2026 · L5 · plan: `docs/plans/L5-configurations-and-design-tables.md`

## Questions

1. What may a configuration change?
2. Where do configurations live, and how does a document build one?
3. How are values written: numbers, or expressions?
4. When is a configuration refused?
5. What do caches and hashes see?
6. What is a design table, and who owns its format?

## Decisions

**Only the numbers a link may drive.** A configuration gives values to
variables of the root's or a composite's layout and to volumes' parameters:
the fixed inputs a volume receives from outside, the same set
`model.link.bad_target` allows. That keeps design rule 1 without a new
reference rule (nothing inside contents can be driven; a parametric
volume's own numbers follow its parameters, a procedure's are its volume's
parameters by name), and it is what the spec asks for: procedure
parameters "driven by global variables or configurations". Configurations
belong to the document, not to a level, so one may drive numbers at every
depth (unlike a link, which belongs to the level of the volumes it joins).
Left out: seeds, suppression and structural variants (they change what
`check()` checks, so every configuration would need its own check),
template parameters (the generator sets them) and outputs as cells (the
CLI's `configs --build` reports them instead).

**In the document, with the active one saved.** `Document::configurations`
and `Document::active_configuration`, both written to JSON only when set,
so every earlier document, content hash, cache key and golden value is
unchanged. The active configuration is saved: a project opens showing the
variant it was saved with, and switching it is an edit (one undo step in
the app, a change of the content hash). `model::configured(doc)` is the
document as it builds: the active configuration's values written into
their targets, with the configurations themselves stripped (so the result
is an ordinary document, and applying twice changes nothing).
`model::build()`, `validate::validate()`, the engine and the CLI build
`configured()`; `build_layout()` and `build_contents()` build what they
are given, since the engine calls them on its configured, expanded
document.

**Expressions, evaluated where the target is.** A value is text: a
variable takes it as its expression (in its own table), a parameter as its
expression in its volume's level, exactly as if authored there; a plain
number becomes a parameter's value rather than an expression, so a
configuration that repeats an authored number builds the authored
document bit for bit. Values for several variables of one table are set
together and the table checked once, so a configuration may swap which of
two variables drives the other.

**Refused when it wouldn't build.** `check()` checks every configuration,
not only the active one, so switching is never refused:
`model.configuration.name`, `.target` (not drivable, or driven twice),
`.expression` (doesn't parse, names what its target can't see, or closes a
variable loop: only findings the authored tables don't already have),
`.loop` (a dependency loop through links that the authored document
doesn't have; built only for configurations with expressions) and
`.active`. A value driving a link's target is a warning (`.linked`): the
link's value replaces it once its source generates, as it replaces the
authored expression. Deleting what a value drives isn't refused: the
app's history prunes such values after every edit
(`model::prune_configurations()`), as deleting a volume drops its links.

**Caches see values, never names.** Keys hash the configured document's
inputs, so configurations that agree on a volume's inputs share its
cached contents, memory and disk: switching regenerates only volumes whose
inputs differ, and switching back regenerates nothing (tested on the
bracket, rows of boxes, a procedural volume and a generating composite
whose child count follows a parameter). No new cache structure, and the
engine's last-four layouts serve a few configurations too.

**A design table is the configurations as CSV.** One row per
configuration, one column per driven number, labelled `width`,
`Volume.parameter` (as `rebuild --set` names it) or `Composite/variable`;
an empty cell keeps the authored value. RFC 4180, our own reader and
writer in vs_model (no new dependency). Importing a table replaces the
configurations: rows keep ids by name, missing rows go. The table is not
stored separately: the document's configurations are the table, so there
is one source of truth and no sync.

## Consequences

- Projects with configurations can't be opened by builds older than L5
  (unknown keys are ignored, so they open showing the authored values;
  the next save drops the configurations). Projects without them are
  unchanged.
- `SubjectKind::Configuration` is appended to vs_core's subject kinds;
  the disk cache's reader accepts it.
- The Qt part (a configuration combo box, the design table dialog, the
  inspector's marks) is built on the Windows desktop (`vs_app_ui`,
  `design_table_dialog.*`, `MainWindow`); everything it calls
  is in `vs_app_core` (`configurations.hpp`) and tested on every CI job.

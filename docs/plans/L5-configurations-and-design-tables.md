# L5 — Configurations and design tables (plan)

Nov 22, 2026 · follows H (hardening) · decision record:
`docs/decisions/0037-configurations-and-design-tables.md`

Status: done. Every part is built: the document, design tables, the
engine, the CLI, the app core, and on the Windows desktop the Qt
toolbar, design table dialog and inspector marks.

## Goal

One project, several variants. The spec's "Later" list has
"Configurations and design tables driving procedure parameters", and its
procedural section says a graph's exposed parameters "can be driven by
global variables or configurations". A rack 3, 4 or 5 cells wide, a
bracket in a thin and a heavy version, an infill at three wall
thicknesses: today each is a copy of the project, or a hand edit before
every export.

A **configuration** is a named set of values for the numbers a document
already exposes to be driven from outside a volume: the layout's
variables (the root's and each composite's) and volumes' parameters,
which is what procedures see. A **design table** is all configurations at
once, as a table: one row per configuration, one column per driven
number, read from and written to CSV so a spreadsheet can author it.

## What a configuration may drive

What a link may drive (`model.link.bad_target`'s rule): a variable of
the root layout or of a composite's layout, or a volume's parameter.
Configurations belong to the document, not to a level, so one row may
drive numbers at every depth. That is the set of fixed inputs a volume receives from
outside, so a configuration never reaches into contents (design rule 1)
and needs no new reference rule. A parametric volume's own variables are
driven through its parameters, as today; a procedure's through the
volume's parameters, which reach the graph by name.

Left out, for now: seeds (not numbers authored with expressions),
suppression of features or volumes (changes the document's structure, so
the checks would have to run per configuration), template parameters of
generated children (the generator sets those), and published outputs as
cells (a design table that reads results back is a report: the CLI prints
one).

## The model (vs_model)

```cpp
struct ConfigurationValue { Id target; std::string expression; };
struct Configuration {
  Id id;
  std::string name;
  std::vector<ConfigurationValue> values;  // one per target at most
};
struct Document {
  ...
  std::vector<Configuration> configurations;  // written only when set
  Id active_configuration;                    // null: the authored values
};
```

`configuration.hpp`:

- `apply_configuration(doc, id)`: a copy with each value written into its
  target: a variable takes the text as its expression; a parameter takes
  a plain number as its value and anything else as its expression. The
  copy keeps the configurations but has no active one.
- `configured(doc)`: the document as it builds: the active configuration
  applied, or the document itself when none is active.
- `prune_configurations(doc)`: drops values whose target is gone (what the
  app's remove commands call, as removing a volume drops its links).

Values are expressions, evaluated where the target is: a variable's in
its table, a parameter's in its volume's level, so a row can say
`width = 2 * depth`. An empty value isn't stored: the authored value
stands.

**Checks** (`check()`, new codes):

| Code | When |
| --- | --- |
| `model.configuration.name` | empty, or two configurations share a name |
| `model.configuration.target` | a value's target isn't a variable of the root's or a composite's layout or a volume's parameter, or one configuration drives a target twice |
| `model.configuration.expression` | a value doesn't parse, names something its target can't see, or closes a variable loop (the variable table's own diagnostics follow it) |
| `model.configuration.loop` | applying it closes a dependency loop the authored document doesn't have |
| `model.configuration.active` | the active configuration doesn't exist |
| `model.configuration.linked` (warning) | a value drives a link's target: the link's value wins once its source generates |

So every configuration of a document that passes `check()` builds, and
switching between them is never refused.

**Build.** `model::build()` builds `configured(doc)`; `build_layout()`
and `build_contents()` build what they're given (the engine and
`validate()` pass the configured document). `layout_inputs()` and every
cache key see only values, never which configuration produced them: two
configurations that agree on a volume's inputs share its cache entry.

**Files.** `"configurations": [{"id", "name", "values": [{"target",
"expression"}]}]` and `"active_configuration"`, both left out when empty,
so every existing project, content hash and golden value is unchanged.
Switching the active configuration changes the content hash (it is saved,
as the shown variant).

## Design tables (vs_model `design_table.hpp`)

- `design_table(doc, all)`: columns (target id, label, authored
  expression) and rows (configuration name, a cell per column, empty when
  not overridden). Columns are the targets some configuration drives, or
  with `all` every drivable target, in document order: root variables,
  then per volume its parameters and, for a composite, its layout's
  variables.
- Labels: a root variable by its name (`width`), a parameter as
  `Volume.parameter` (as `rebuild --set` names it), a composite's
  variable as `Volume/variable`. Volume names are unique in an authored
  document, so labels are too.
- `to_csv(table)` / `from_csv(text)`: RFC 4180 (quotes, doubled quotes,
  CRLF or LF), header `configuration,<labels...>`.
- `apply_design_table(doc, table)`: the table becomes the configurations:
  rows matched by name keep their ids, new rows get new ones, missing
  rows are removed (and the active one cleared if it was), and a row's
  values are exactly its non-empty cells. Unknown or repeated columns,
  repeated or empty row names: `model.design_table.column` / `.row`;
  bad CSV: `model.design_table.syntax`. The result still has to pass
  `check()` (the app and CLI go through `edit()`).

## Engine (vs_eval)

`evaluate()` checks the authored document, then evaluates
`configured(doc)`: `Evaluation::document` is the configured document.
Nothing else changes: keys hash values, so switching configurations
regenerates only volumes whose inputs differ, and switching back finds
everything in the cache (memory and disk). `Evaluation::configuration`
names the configuration evaluated.

## CLI

- `vstudio-cli configs <project> [--build] [--threads N]`: lists the
  configurations and their values; with `--build`, evaluates each (one
  engine, so shared volumes come from the cache) and prints status and
  published outputs per configuration, plus how many volumes each
  regenerated.
- `vstudio-cli design-table <project> [--all] [--out table.csv]` writes
  the table; `design-table <project> --import table.csv [--out project]`
  reads one back (checked; saved to the project, or to `--out`).
- `--config <name>` on `rebuild`, `validate`, `export` and `info`: use
  that configuration instead of the active one.

## App

- `vs_app_core`: commands `add_configuration(name, from)` (a copy of an
  existing one, or empty), `rename_configuration`, `remove_configuration`,
  `set_configuration_value(config, target, expression)` (empty removes
  it), `set_active_configuration(config)` and `set_design_table(table)`,
  each one undo step; removing variables, parameters and volumes prunes
  values. `configurations.hpp` (no Qt): the design table as the dialog's
  rows and columns, and which inspector values the active configuration
  overrides.
- Qt (Windows desktop): a Configuration combo box on the toolbar (the
  authored values, then each configuration), Model > Design Table… (a
  grid: add, rename, remove rows; add columns from the drivable targets;
  import and export CSV), and the inspector marking a parameter or
  variable the active configuration overrides.

## Tests (acceptance)

1. JSON round-trips configurations; documents without them, their content
   hashes and every golden value are unchanged.
2. `apply_configuration()` writes root variables, composite variables and
   parameters (numbers as values, expressions as expressions); `check()`
   reports every code above, and accepts the bracket, nested and rack
   fixtures with configurations on each kind of target.
3. Design tables round-trip through CSV (quotes, commas, empty cells),
   and `apply_design_table()` keeps ids, adds, removes and refuses as
   described.
4. Engine: on the bracket, switching between configurations regenerates
   only the volumes whose inputs differ, and switching back regenerates
   nothing; Upright (static) never regenerates.
5. vs_proc: a configuration drives a procedural volume's parameter (the
   mesh changes; back, the cache answers), and a generating composite's
   parameter changes how many children its generator makes.
6. CLI: `configs --build`, `design-table` export and import, `--config`.
7. App core: every command undoes to identical hashes; removing a volume
   or variable prunes values; the inspector's overrides.
8. App (Windows, `[gui]` where it renders): the combo box switches the
   shown configuration; the design table dialog edits and imports.
   Built: `apps/vs_app/tests/test_configurations_ui.cpp` (the dialog on
   its own, then the box and Model > Design Table… in the window, `[gui]`).

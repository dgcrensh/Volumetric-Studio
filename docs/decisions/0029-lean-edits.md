# 0029: Leaner edits: reused layouts, refreshed parameters, one check

Nov 22, 2026 · follows 0028 · before H6 and H7

## Context

An edit in the app runs `History::apply()` (M14), then the session's
preview tier and full evaluation (M16). Outside generation, each of those
did whole-document work:

- `History::apply()` ran `model::check()` on the new document **and** on
  the current one, every edit, to tell new errors from old.
- `Engine::evaluate()` ran `check()` again, then per wave `apply_links()`
  (a document copy) and, whenever a link value changed (and once at the
  start), `build_layout()` and `validate_layout()` of the whole document,
  then `cache_key()` for each ready volume (a `shape_hash()` of its
  boundary and patches, and the JSON of its tree). The preview and the
  full evaluation of one edit repeated all of it, and a linked document
  built its layout at least twice per evaluation even when no value
  changed since the last one.

The constraint: identical results. Every evaluation, key, output hash,
diagnostic and golden hash must stay what it is.

## Where the time went

`vstudio-cli bench-edit` (new) builds rows of parametric boxes with two
links from V0's volume: one to V1's parameter, one to the layout variable
`w` that sets the last box's height. It times History's checks, the
preview tier and the full evaluation for three kinds of edit (V2's
contents, V3's parameter, V0's parameter: upstream of both links), and
`--project` times a saved project. 4-core cloud container, Ubuntu 24.04,
GCC 13.3, release, 4 engine threads; times vary about ±10 % run to run.

At 400 volumes, before (`--no-reuse`, which also checks twice per edit):
a contents edit took 18 ms in History, 455 ms preview, 483 ms full. Probing
the engine: **layout builds about 70 %** (165 ms each, two per
evaluation; `make_box` and the boundary's faces per volume, the layout's
own tree only 0.2 ms), **cache keys about 18 %** (62 ms, of which 35 ms
`shape_hash()` of boundaries), checks about 2 % each (8 ms), the
dependency graph 2 %, `validate_layout()` 1 % (5 ms; it dominates on
documents whose volumes touch, 11 ms of the nested sample's 22). On the
bracket the layout's tree is the cost (9 of 9.5 ms) plus 5 ms of checks.

So the user's two observations are right, and the layout is the bigger of
them: the second `check()` is about 2 % of an edit.

## Candidates

1. **One check per edit in History.** Snapshots are immutable: keep the
   current state's findings with it (the new state's, found while
   accepting it), and check only the new document. Undo and redo change
   the current state, so the findings are keyed by the snapshot pointer.
2. **A faster `check()`.** Index ids once per check instead of the linear
   `find_*` and `find_tree_id()` lookups of `resolve()`; 100 → 400
   volumes went 1.1 → 7.7 ms, mildly superlinear.
3. **Incremental check.** Re-check only what an edit touched. Dependency
   loops and encapsulation are global; the risk is out of proportion to
   2 %.
4. **Reuse whole layouts (a memo).** A layout depends only on the
   document's layout inputs: everything but leaf volumes' contents (and
   their outputs and seeds). Keep the last few layouts by those inputs and
   take an equal one. Hits the preview → full repeat, every wave whose
   link values are what they were, and every contents edit.
5. **Refresh parameters.** When the inputs differ only in leaf volumes'
   parameter values (a parameter edit, or a link into a parameter), take
   the kept layout and evaluate those volumes' parameters again; nothing
   else in the layout or its checks reads them.
6. **Seed link values from the last evaluation.** Start with the last
   evaluation's link values so a linked document builds once. Needs care
   when a source fails (the seeded value must go again) and 4 subsumes it.
7. **Per-volume reuse inside `build_layout()`.** Rebuild only the
   boundaries whose inputs changed. Prototyped for boxes only, as a memo of
   `make_box()` by its corners.
8. **Keep the shape hashes.** `cache_key()` hashes each boundary's exact
   shape; with 4 the shapes are the same objects, so hash each once per
   kept layout.
9. **Reuse the engine's check.** The preview and the full evaluation check
   the same document: keep the last one checked and its findings, compared
   by equality.

4 and 7 compete for the layout (a whole-layout memo, or partial rebuilds);
5 refines 4; 1, 8 and 9 stand alone.

## Measurements

ms per edit at 400 volumes (mean of 3–10; preview / full; the layouts
column is layouts *built* per evaluation):

| Variant | contents | parameter | upstream |
| --- | --- | --- | --- |
| before | 455 / 483 (2, 2) | 440 / 447 (2, 2) | 288 / 483 (1, 2) |
| 4: whole-layout memo | 103 / 109 (0, 0) | 434 / 99 (2, 0) | 262 / 308 (1, 1) |
| 4 + 5: and refreshed parameters | 100 / 108 (0, 0) | 102 / 109 (0, 0) | 113 / 316 (0, 1) |
| 7 alone (boxes only) | 123 / 139 (2, 2) | 116 / 123 (2, 2) | 105 / 131 (1, 2) |
| 4 + 5 + 7 | 101 / 112 | 105 / 114 | 104 / 116 |
| 4 + 5 + 8 | 54 / 61 | 56 / 60 | 55 / 248 |
| 4 + 5 + 8 + 9 (**chosen**) | 56 / 53 | 57 / 54 | 60 / 295 |

History's part (1): 17 → 9 ms at 400 volumes (one check instead of two).

- **4 is the big step** and costs only document copies and comparisons
  (about 2 ms at 400 volumes). On the bracket, evaluating again with
  nothing changed went from 28 ms to 0.16 ms; on the nested sample from
  22 ms to 2.4 ms.
- **5** turns parameter edits and parameter links into hits.
- **7 alone** helps every kind of box-layout edit, including upstream
  ones, but it leaves the rest of the layout and its checks running, does
  nothing for feature boundaries (the bracket's layout cost is its tree),
  and a general version needs the layout tree's partial rebuild and the
  boundaries' dependencies tracked through keep-outs and levels. With 4
  and 5 in place it only helps edits that really change the layout
  (upstream: 316 → 116 ms). Not built; see "Later".
- **8** halves what is left: keys were most of it.
- **9** saves the full evaluation's check (6–8 ms at 400 volumes) for a
  document copy on the preview; small, kept because it is cheap.
- **2 and 3** weren't built: after 1 and 9 an edit checks one document
  twice (History, the preview), about 16 ms of the remaining ~120 ms at
  400 volumes.

Whole edits (History + preview + full), `bench-edit`, before → after:

| Volumes | contents | parameter | upstream |
| --- | --- | --- | --- |
| 20 | 50 → 10 ms | 54 → 7 ms | 50 → 22 ms |
| 100 | 245 → 29 ms | 261 → 29 ms | 213 → 79 ms |
| 400 | 956 → 135 ms | 903 → 143 ms | 792 → 347 ms |

## Decision

Take 1, 4, 5, 8 and 9.

- `History` keeps `model::check()` of one snapshot (`checked_`), set to the
  new state's findings when `apply()` keeps it and recomputed when undo,
  redo or `reset()` makes another snapshot current. `try_apply()` checks
  only the new document.
- `model::layout_inputs(doc)`: the document with every non-composite
  volume's contents emptied (same kind), outputs and seed cleared.
  `model::layout_shape(doc)`: that, with those volumes' parameter values
  cleared too. `model::refresh_parameters(doc, layout, i, settings)`
  evaluates volume `i`'s parameters at its level from a finished layout,
  and refuses (returns false, changes nothing) for a composite, a failed
  volume, or a parameter with a diagnostic now or in the old layout, so
  the build's diagnostics never need editing.
- The engine builds every layout **from `layout_inputs()`**, so the memo is
  correct by construction: equal inputs, equal build. It keeps the last 4
  layouts (`build`, the `validate_layout()` report, shape hashes, inputs
  and shape); `layout_of()` takes an equal one, else refreshes one equal
  in shape, else builds. `EvaluationStats::layouts_reused` counts the
  first two; `layout_builds` counts real builds.
- `eval::LayoutShapeHashes` computes each boundary's and patch's
  `shape_hash()` the first time a key needs it; `cache_key(..., hashes)`
  gives the same key as before.
- The engine keeps the last document it checked and its findings.
- `EngineOptions::reuse_layouts` (default on) turns all of it off: layouts
  built from the document itself and checked every time, as before. Tests
  run both engines side by side; `bench-edit --no-reuse` measures the
  difference.

Sharing: a reused layout's solids are the same handles as the last
evaluation's, as cached contents already were. Readers that might fill
OCCT caches (tessellation, classifiers, containment) already work on
copies (H2), and evaluations are serialised by the engine's mutex.

Memory: four layouts and two document copies each (without leaf
contents), plus one full document for the check: kilobytes to a few
megabytes, and the boundaries were alive in the shown evaluation anyway.

## Later

- **7, done properly**, if layout edits on large documents become the
  bottleneck: partial layout rebuilds (the tree's `rebuild()` from the
  first changed feature, boundaries whose resolved inputs are unchanged,
  `validate_layout()` only for pairs that moved).
- Cache keys still serialise each parametric tree to JSON per evaluation
  (about a third of what is left at 400 volumes).
- `check()` is mildly superlinear (linear `find_*` lookups); index ids if
  documents grow past the thousands.

## Tests

`libs/vs_eval/tests/test_layout_reuse.cpp` drives an engine that reuses
layouts and one that doesn't through the same edits on the chain of
links, the bracket and the nested sample (contents, leaf and composite
parameters, a parameter that fails and is fixed, layout variables, a
moved box, a link target's authored value), preview and full each time,
and compares everything the evaluations show as text: statuses, keys,
diagnostics, the layout report, boundaries' shape hashes, faces,
parameters, patches, output hashes and the evaluated document. It also
checks that `build_layout()` and `validate_layout()` of `layout_inputs()`
equal those of the document, and that `refresh_parameters()` equals a
build or refuses. `apps/vs_app/tests/test_history.cpp` checks that undo
and redo bring each state's own errors back. Every existing suite passes
unchanged (668 tests, golden hashes included), and the vs_eval suite on 1
and 16 threads.

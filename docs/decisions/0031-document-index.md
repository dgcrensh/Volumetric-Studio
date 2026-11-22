# 0031: Index the document once per pass

Nov 22, 2026 · follows the rack benchmark (decision 0030) and lean edits (0029)

## Context

`bench-rack` (0030) found the preview of a rack of 10,000 generated cells
did not finish in 27 minutes: about 15 of them went to the `check()` run
after `expand()`, inside `DependencyGraph::build()`. Every lookup in
vs_model is a linear search of the document's lists:
`Document::find_volume()`, `model::joins()` (which walks a volume's
parents, each by `find_volume()`), `classify()` (which walks every id of
every tree), `delegate_of()`, `children()`. One lookup per volume is
quadratic; the graph asks `joins()` for every surface and volume, which is
cubic in cells. `vs_validate_tests "[.scaling]"` (this decision) times the
layout phases on their own, a grid of n × n cells expanded by hand:

| cells | check() before | of it, the graph |
| ---: | ---: | ---: |
| 1,024 | 1.75 s | |
| 2,500 | 25.8 s | 11.6 s |

Callgrind at 1,024 cells put 81% of `check()` in `find_volume()` under
`joins()` and `ancestors()`.

## Candidates

1. **A lookup cache inside `Document`.** `find_volume()` and friends keep
   an id → index map, rebuilt when it no longer matches (each hit is
   verified against the list). No caller changes.
2. **A `DocumentIndex` built once per pass.** A read-only object over a
   `const Document&`: hash maps of ids, each level's children, each
   volume's joining surfaces and keep-outs, delegations, and `classify()`'s
   targets, all built in one walk. Callers that look up a lot (`check()`,
   the dependency graph, cache keys, the layout checks, the engine) build
   one and ask it instead of the document.

## Measured

`check()` on the expanded grid, the cache (1) measured at 10,000 cells
only, where it was already clear:

| cells | before | 1 | 2 |
| ---: | ---: | ---: | ---: |
| 1,024 | 1.75 s | | 0.028 s |
| 2,500 | 25.8 s | | 0.084 s |
| 10,000 | about 30 min, projected | 50.6 s | 0.64 s |

1 stayed quadratic: verifying hits, `classify()` (which isn't a
`find_volume()`), and `joins()` asked surface by volume still scanned.
It also hid mutable state behind a `const Document&` that several threads
read (`build_contents()`, H2), which needs locks or atomics on every
lookup. Rejected; 2 is what this decision does.

## What changed

- **`model::DocumentIndex`** (`index.hpp`). Answers exactly what the
  list searches answer, including on malformed documents (repeated ids:
  the first wins, as the searches find it; parent loops; delegations to a
  grandchild). The walks themselves (`ancestors`, `is_within`,
  `child_toward`, `level_of`, `joins`, `resolve`, `check_variable_name`)
  are written once as templates over a lookup (`src/lookup.hpp`) and run
  over either the document (`detail::Scan`) or the index, so the two can't
  drift. `test_index.cpp` compares every answer, every id from every
  scope, on the bracket, curved, nested and rack fixtures, an expanded
  rack and five malformed documents.
- **`check()`** builds one index and uses it throughout; duplicate names
  and link targets are hash sets. **`DependencyGraph::build(index)`**;
  `find()` is a map, `order()` a heap (same order), and `upstream(node)`
  gives every node upstream in one walk, which the engine uses instead of
  `depends_on()` for every pair of volumes.
- **`FeatureTree::subjects()`** labels every id of a tree in one pass
  (`tree_ids()` labelled each with a search), and `append()` adds many
  features with one duplicate check. `expand()` appends a composite's
  generated points and planes at once and serialises each template once.
- **`cache_key(index, ...)`** reads `joining()` instead of asking
  `joins()` for every surface; the engine keeps an index of the evaluated
  document (made again when links change it).
- **`validate_layout()`** sweeps boxes along x for candidate pairs (as
  before, sorted, so the same pairs in the same order), labels subjects
  from per-level tables, and takes an optional `ParallelFor`
  (`parallel.hpp`, moved from `containment.hpp`). Pairs go in chunks of
  32 and surfaces in chunks of 16, each chunk with its own deep copies of
  the boundaries (OCCT's caches, H2), folded in order: the report is the
  same on any number of threads (`test_scaling.cpp`). The engine passes
  its pool.

No golden hash, cache key, diagnostic or report changes; all 672 tests
pass unchanged.

## Results

`vs_validate_tests "[.scaling]"`, Linux cloud session, 4 vCPUs, GCC 13.3,
`linux-gcc-release`, Nov 22, 2026 (seconds; `validate_layout()` on one
thread and on four):

| cells | expand | check | graph | layout | checks, 1 | checks, 4 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | 0.010 | 0.002 | 0.001 | 0.09 | 0.53 | 0.37 |
| 1,024 | 0.079 | 0.028 | 0.015 | 0.84 | 6.54 | 4.37 |
| 2,500 | 0.226 | 0.084 | 0.049 | 2.44 | 15.5 | 11.1 |
| 5,041 | 0.537 | 0.210 | 0.128 | | | |
| 10,000 | 0.858 | 0.635 | 0.376 | | | |

Every phase is now linear in cells. `expand()` at 10,000 cells went from
about 4 s to 0.9 s.

`bench-rack --cells 10,100,1000,10000 --threads 4` on the same machine
(`docs/benchmarks/bench-rack-2026-11-22-linux-index.csv`),
against decision 0030's table (after 0029):

| cells | preview ms, 0030 | preview ms, now | again ms, now | peak MB |
| ---: | ---: | ---: | ---: | ---: |
| 10 | 61 | 60 | 3.5 | 22 |
| 100 | 681 | 558 | 21 | 29 |
| 1,000 | 9,931 | 5,849 | 229 | 85 |
| 10,000 | did not finish in 27 min | 74,356 | 4,707 | 642 |

Time now grows as cells^1.0 to cells^1.1. At 1,000 cells, children:
expand and check went from 893 ms to 139 ms, dependencies from 785 ms to
32 ms, keys and cache from 853 ms to 231 ms, and layout and checks (one
phase since 0029) from about 7,160 ms to 5,434 ms. At 10,000 cells the
preview is 92% layout and checks (68.4 s); expand and check, dependencies
and keys take 1.5 s, 1.5 s and 2.9 s.

## The layout checks

They are linear now and the largest phase left: about 6 ms a cell on one
thread, nearly all of it in OCCT (`common` for overlaps, face distance
for contacts, the patch rules). On 4 threads they run 1.6× faster; four
separate processes scale nearly 4×, so something inside OCCT serialises
threads (stacks point at its exception handling setup). `MMGT_OPT` made
no difference. This was time-boxed and left open.

Rejected because they change reports:

- **An area bound before `common_faces`**, skipping pairs whose faces
  can't reach the tolerance: exact on boxes, not on curved faces.
- **Analytic fast paths for box-shaped boundaries.** Most generated cells
  are boxes, and two boxes' overlap and contact are arithmetic. But the
  numbers (contact areas, `validate.contact.edge`'s point) would differ
  from OCCT's in the last bits, and report text is compared in tests and
  caches. Worth doing if exactness to OCCT is relaxed for boxes; a
  product call.

## Follow-ups

- **Diagonal neighbours.** A grid's diagonal cells touch along an edge,
  and each pair is a `validate.contact.edge` warning: 2(n − 1)², about
  19,600 at 10,000 cells. Whether generated children of one composite
  should warn about each other at all is a product question.
- **Quadratic spots left**, none on the 10,000-cell path today: vs_proc's
  `gather_inputs()` scans surfaces per procedural volume,
  `Document::link_subject()` scans volumes, `check_variable_name()` scans
  for an unknown name, the engine's `generated_owner()` per error
  subject, `DependencyGraph::dependants()`, and the fallbacks of
  `uses_layout()`.
- The layout checks' thread scaling inside OCCT (above).

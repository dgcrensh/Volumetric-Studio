# 0030: A preview scaling benchmark on the rack fixture

Nov 22, 2026 · a baseline for making document edits leaner (decision 0029)

## Context

`bench-gyroid` times one large procedural volume. Nothing measured how the
preview tier (`Engine::preview()`: check, generated children, layout,
layout checks, keys) grows with the number of volumes, which is what
generated children (L4) push up to `children.grid`'s 10,000.

## Decision

- `vstudio-cli bench-rack [--cells N[,N...]] [--cell <mm>] [--threads N]
  [--csv <file>]` builds the rack fixture's shape (`libs/vs_model/tests/rack.*`:
  a Base under a Rack composite with a Cell template and a bonded Seat
  delegated to the corner cell) at any size: `children.grid` makes
  nx x ny cells (as square as the count allows) of `--cell` mm (8, so
  10,000 cells fit the working space), centred on the origin. The CLI
  builds it itself, since the fixture library is test-only and fixed at
  60 x 40 mm.
- For each size it runs `preview()` on a fresh engine, then again on the
  same engine (the generated layout cached), and prints each phase's
  time, share and peak RSS, plus how much the peak grew over the RSS when
  the phase started (Linux; `memory.*`, shared with `bench-gyroid`).
- The engine reports phases as `ProgressEvent::Kind::PhaseFinished` with
  a static `phase` name (`cancel.hpp` lists them). Phases run on the
  calling thread one after another, so the time between two events is a
  phase's; phases that repeat (per wave, per generating composite) add up.
  Existing progress listeners filter on kind and are unaffected.
- ctest runs it at 6 and 24 cells.

## First results

Linux cloud session, 4 threads, GCC 13.3, `linux-gcc-release`, Nov 22,
2026 (`docs/benchmarks/bench-rack-2026-11-22-linux.csv`):

| cells | volumes | surfaces | preview ms | ms per cell | again ms | peak MB |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 12 | 14 | 74 | 7.4 | 69 | 22 |
| 100 | 102 | 181 | 650 | 6.5 | 685 | 26 |
| 1,000 | 1,002 | 1,936 | 9,703 | 9.7 | 9,623 | 67 |
| 10,000 | 10,002 | ~19,800 | did not finish in 27 min | | | |

At 1,000 cells: layout checks 6,381 ms (66%), children: expand and check
893 ms, keys and cache 853 ms, dependencies 785 ms, layout 783 ms. From
100 to 1,000 cells, layout checks grow about 13x, but expand and check
39x and dependencies 240x: those are the superlinear ones.

At 10,000 cells, stack samples showed about 15 minutes in the `check()`
after expansion, inside `DependencyGraph::build()`
(`GraphBuilder::add_edges` → `model::joins()` → `ancestors()` →
`Document::find_volume()`, a linear search per call, for each surface and
volume), then the engine's own `DependencyGraph::build()` and its
pairwise `depends_on()` loop. The run was stopped at 27 minutes.

A preview again on the same engine cost the same: only the generator
run was cached, and it was never the cost.

The table above predates decision 0029 (layouts and checks reused across
edits), which landed first; since then `layout` and `layout checks` are
one phase, `layout and checks`. With 0029 on the same machine the first
preview is unchanged (61 ms, 681 ms, 9,931 ms at 10, 100 and 1,000
cells) and the preview again drops to 3 ms, 26 ms and 2,850 ms.

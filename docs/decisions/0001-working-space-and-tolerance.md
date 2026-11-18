# 0001: Working space and tolerance hold in OCCT

Nov 18, 2026 · M1

## Question

The spec proposes a ±1,000 mm working space with an absolute linear tolerance
of 1e-6 mm, and asks (open question 1) whether Open CASCADE's tolerance model
copes with that. OCCT stores a tolerance on every vertex, edge and face, and
operations can grow them; if they grow past the product tolerance, the
studio's touching and containment checks stop meaning what they say.

## What was measured

`run_tolerance_spike()` in `vs_kernel` (rerun with `vstudio-cli
tolerance-spike`, asserted by `vs_kernel_tests "[spike]"`) builds ten parts,
each once centred on the origin and once in the 100 mm cube touching the
(+1000, +1000, +1000) corner. For each it records validity, the volume against
the analytic value, and the largest vertex, edge and face tolerance.

| Case | What it stresses |
| --- | --- |
| box | baseline primitive |
| drilled | cut with coplanar caps (hole flush with both faces) |
| drilled-25 | 25 sequential cuts, to look for accumulation |
| sphere-union | sphere/plane intersections |
| crossed-cylinders-union / -common | cylinder/cylinder intersection curves (Steinmetz solid) |
| min-feature-boss | a boss of the minimum feature size (1e-3 mm) on a 100 mm cube |
| fillet-12 | rolling-ball fillets on all twelve edges, meeting at corners |
| near-coincident | two bodies 0.5e-6 mm apart, united |
| near-coincident-fuzzy | the same, with the product tolerance as the boolean's fuzzy value |

Results (OCCT 8.0.1, Windows, MSVC 14.44, RelWithDebInfo):

| Case | Valid | Solids | Volume rel. error | Max tolerance (mm) |
| --- | --- | --- | --- | --- |
| box | yes | 1 | 2e-16 | 1e-7 |
| drilled | yes | 1 | 0 | 1e-7 |
| drilled-25 | yes | 1 | 2e-15 | 1e-7 |
| sphere-union | yes | 1 | 2e-16 | 1e-7 |
| crossed-cylinders-union | yes | 1 | 5e-13 | 1e-7 |
| crossed-cylinders-common | yes | 1 | 2e-12 | 1e-7 |
| min-feature-boss | yes | 1 | 1e-16 | 1e-7 |
| fillet-12 | yes | 1 | 2e-16 | 1e-7 |
| near-coincident | yes | **2** | 8e-16 | 1e-7 |
| near-coincident-fuzzy | yes | 1 | 2e-16 | 1e-7 |

The origin and corner rows agree to the digits shown (topology counts are
identical; volume errors differ only in the last bits), so only one row per
case is listed.

## Findings

1. **Position doesn't matter.** At 1,000 mm a double has about 1e-13 mm of
   resolution, seven orders of magnitude finer than the tolerance. Nothing in
   the results depends on where in the working space a part sits.
2. **Tolerances did not grow.** Every vertex, edge and face in every case kept
   OCCT's default of 1e-7 mm (`Precision::Confusion()`), ten times inside the
   product tolerance, including the curved intersections and the fillets.
3. **Volumes are exact to 1e-11 or better** for parts with analytic faces.
   For B-spline faces (a sphere under a non-uniform scale) OCCT's default
   volume integration was off by 4e-4; `mass_properties()` now uses the
   adaptive integration with a 1e-10 relative bound, which fixes it.
4. **The minimum feature size works**, but a size computed as `max - min`
   can land a rounding error below 1e-3 mm, so the size check allows the
   linear tolerance.
5. **OCCT's booleans work at 1e-7, not 1e-6.** Two bodies 0.5e-6 mm apart
   stay two bodies after a plain union. The spec counts such a gap as
   touching. Passing the product tolerance as the boolean's fuzzy value
   (`BooleanOptions::fuzzy_mm`) closes it and leaves tolerances at 1e-7.

## Decision

- **The open question is settled: keep ±1,000 mm and 1e-6 mm.** OCCT copes
  with both, with a factor of ten between its working tolerance and ours.
- The product tolerance is the studio's notion of "touching", applied by the
  studio's own checks (`min_distance`, `classify`, and in M6 the overlap and
  contact checks), not by changing OCCT's precision.
- Booleans default to OCCT's exact behaviour. Callers that combine bodies
  meant to touch (for example bonded volumes, or user geometry that is only
  "close enough") pass `fuzzy_mm = linear_tolerance_mm`. M3 decides which
  features do so.
- `audit_tolerances()` runs after every feature from M3 on, as the spec asks.
  These cases did not grow tolerances, but the operations most likely to
  (sweeps, lofts, shells and offsets in M4, and STEP/IGES imports in M9) are
  not covered yet. M4 adds them to the spike and revisits this record if any
  case exceeds 1e-6 mm.

## Consequences

- `ProductSettings` defaults stay as they are; the spec's two open questions
  on working space and tolerance can be ticked.
- Linux numbers come from CI; the assertions in `test_tolerance_spike.cpp`
  (valid, analytic volume to 1e-6, no tolerance above 1e-6, same outcome at
  origin and corner, fuzzy closes the gap) must hold there too.

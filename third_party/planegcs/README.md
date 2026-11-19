# PlaneGCS (vendored)

FreeCAD's 2D geometric constraint solver, used by `vs_sketch` behind its
`SketchSolver` interface. Only `vs_sketch` links it, privately; no public
header outside this directory includes it.

- **Source:** FreeCAD 1.1.4, `src/Mod/Sketcher/App/planegcs`
  (https://github.com/FreeCAD/FreeCAD/tree/1.1.4/src/Mod/Sketcher/App/planegcs).
- **Licence:** LGPL 2.1 or later (`LICENSE`, FreeCAD's). The studio is never
  distributed, so the licence places no constraint on it.
- **Dependency:** Eigen 3 (the vcpkg `sketch` feature).

## Changes from upstream

FreeCAD builds PlaneGCS inside its Sketcher module, with Boost, FreeCAD's
console and precompiled headers available. Here it builds on its own:

1. `src/*.h`: `#include "../../SketcherGlobal.h"` became
   `#include "SketcherGlobal.h"`, which resolves to `shim/SketcherGlobal.h`.
2. `src/Geo.h` and `src/Constraints.cpp`: two unused Boost includes removed.
3. `src/GCS.cpp`: `System::initSolution` split the system into connected
   components with `boost::connected_components`. A union-find does the same
   job now, numbering components in the same order, so Boost isn't needed.
4. `shim/` supplies the FreeCAD headers PlaneGCS still includes:
   `SketcherGlobal.h` (an empty export macro, plus `<cassert>` and `<cmath>`,
   which upstream gets transitively), `FCConfig.h` (empty) and
   `Base/Console.h` (a console whose calls do nothing; `vs_sketch` reports
   through diagnostics instead).

`vstudio.patch` is the full diff of items 1 to 3 against the upstream files.

## Building

`CMakeLists.txt` makes the static library `planegcs` (alias
`vs::planegcs`). It compiles with warnings off and never with warnings as
errors, since it is not first-party code, but keeps floating-point
contraction off like the rest of the studio so solves are deterministic.

## Updating

1. Download the files listed in `src/` from the new FreeCAD tag.
2. Apply `vstudio.patch` (`git apply -p1 --directory=third_party/planegcs/src`
   or by hand); re-check for new Boost or FreeCAD includes with
   `grep -n "boost\|Base/\|FCConfig" src/*`.
3. Update the tag above, rebuild and run `ctest -L vs_sketch`.

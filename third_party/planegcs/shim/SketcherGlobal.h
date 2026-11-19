// vstudio shim for FreeCAD's src/Mod/Sketcher/SketcherGlobal.h.
// PlaneGCS is built as a static library here, so there is nothing to export.
#pragma once
#define SketcherExport

// In FreeCAD, <cassert> and <cmath> reach PlaneGCS through Boost and the
// precompiled headers. Every PlaneGCS header includes this one, so they are
// supplied here instead of patching each source.
#include <cassert>
#include <cmath>

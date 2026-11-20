#pragma once

// Documents with curved connection surfaces (L2), shared by the suites.
//
//   Shaft in sleeve   a shaft of radius `radius` (z 0..height) inside a
//                     sleeve whose bore is `radius + gap` and outside
//                     `outer`, joined across a band round the z axis on
//                     the bore, from `patch_start` for `patch_length`.
//   Half shell        the same, cut in half along the x axis (y >= 0): a
//                     half shaft and a half sleeve joined by a curved
//                     rectangle sweeping half a turn (angle 0 .. π).
//
// Both volumes are placeholders; tests give them contents as they need.

#include "vs/model/document.hpp"
#include "vs/model/project.hpp"

namespace curved {

using vs::core::Id;

struct Options {
  bool half = false;  // the half shell instead of the shaft in a sleeve
  double radius = 10.0;
  double outer = 20.0;
  double height = 30.0;
  double gap = 0.0;  // the bore is radius + gap
  double patch_start = 0.0;
  double patch_length = 30.0;
  vs::model::ConnectionParams params = vs::model::BondedParams{};
};

struct Curved {
  vs::model::Project project;
  Id shaft_sketch, shaft_block, sleeve_sketch, sleeve_block;  // layout features
  Id shaft, sleeve, surface;
};

Curved make_curved(const Options& options = {});

// Parametric contents filling the shaft (or the sleeve) of `options`' layout:
// its profile sketched on the XY plane and extruded the full height.
vs::model::ParametricContents fill(const Options& options, bool shaft);

}  // namespace curved

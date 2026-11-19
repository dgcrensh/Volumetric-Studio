#pragma once

// The sample "bracket" project shared by the vs_model suites: a layout with
// two sibling volumes joined by a bolted flange.
//
//   Base      60 x 40 x 10 block on XY (z 0..10), parametric: its contents are
//             one extrusion sketched on the flange, filling the boundary,
//             `thickness` deep (a parameter driven by the layout's `base_h`).
//             Publishes `mass` (PLA, 1.24 g/cm³) and `volume`.
//   Upright   60 x 10 x 40 block standing on the base (z 10..50), static: a
//             STEP file of the same box. Its parameter `load` is fed by the
//             Base's mass through a link (allowed: Upright is not upstream of
//             Base). Publishes `envelope`.
//   Flange    the 60 x 10 rectangle where they meet (a layout sketch on the
//             datum plane z = base_h), bolted with two M4 bolts 40 mm apart.

#include <string>

#include "vs/model/document.hpp"
#include "vs/model/project.hpp"

namespace bracket {

using vs::core::Id;

struct Bracket {
  vs::model::Project project;
  // layout
  Id width, depth, base_h, upright_t, upright_h;           // variables
  Id base_sketch, base_block, top_plane, upright_sketch,   // features
      upright_block, flange_sketch;
  // model
  Id base, upright, flange, link;
  Id base_thickness, upright_load;        // parameters
  Id base_mass, base_volume, upright_envelope;  // outputs
  Id inner_sketch, inner_extrude;         // inside Base's contents
};

Bracket make_bracket();

// The STEP file of an axis-aligned box, as bytes.
vs::model::Blob step_box(double x0, double y0, double z0, double x1, double y1, double z1);

// The codes of a diagnostics list, for readable assertions.
std::string codes(const vs::core::Diagnostics& diagnostics);

}  // namespace bracket

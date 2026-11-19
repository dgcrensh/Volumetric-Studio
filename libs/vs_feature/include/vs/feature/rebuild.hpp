#pragma once

// Rebuilding a feature tree into a solid.
//
// rebuild() evaluates the variables, then runs the features top to bottom on
// the calling thread. Each feature sees the body as the features above it
// left it. A feature that fails leaves the body unchanged and the rebuild
// carries on, so one broken feature never takes the ones above it down;
// features that refer to a failed one fail too, with
// "feature.reference.upstream_failed". Suppressed features are skipped, and
// referring to one is "feature.reference.suppressed".
//
// Every diagnostic a feature raises names that feature first (the "act
// here" target). Codes:
//
//   feature.reference.broken          the referenced feature, sketch entity or
//                                     face no longer exists
//   feature.reference.forward         the reference is to a feature further down
//   feature.reference.wrong_kind      e.g. extruding something that isn't a sketch
//   feature.reference.suppressed / .upstream_failed   (above)
//   feature.expression.*, feature.variable.*           a Param's expression
//   feature.sketch.unsolved           the sketch doesn't solve (solver findings follow)
//   feature.sketch.no_profile         nothing closed to extrude or revolve
//   feature.datum.degenerate          coincident points, parallel planes, ...
//   feature.extrude.*, feature.revolve.*               bad depths, angles, end planes
//   feature.cut.no_body               a cut with nothing to cut from
//   kernel.*                          the kernel's own errors, passed through
//
// After every feature that changes the body the rebuild runs the kernel's
// tolerance audit (decision 0001) and reports entities above the product
// tolerance as warnings on that feature.
//
// Booleans: every feature combines with the body using the product's linear
// tolerance as the fuzzy value, so faces within the tolerance of each other
// count as touching (decision 0002).
//
// Face names. Features name the planar faces they make, so a sketch or datum
// can sit on them (FaceRef):
//
//   Extrude (add)  "start"         the face on the sketch plane (symmetric: the far
//                                  side behind it), facing back along the extrusion
//                  "end"           the far face
//                  "side:<id>"     the side face made by sketch line <id>
//   Extrude (cut)  "end"           the floor of a blind or up-to-plane cut
//                  "side:<id>"     the wall made by sketch line <id>
//   Revolve        "start", "end"  the flat ends of a partial revolve
//
// A FaceRef is found again by geometry: the referenced feature reports where
// its named face lies in this rebuild, and the reference resolves when the
// body (as the referring feature sees it) still has a planar face on that
// plane, facing the same way. So references follow parameter changes, and
// survive later features that split or trim the face, but break when the
// face is cut away entirely or its sketch line is deleted. The spec accepts
// this fragility inside a volume.
//
// Rebuilding from a changed feature: pass the previous result and the first
// feature that changed. Results above it are reused when the tree above it
// and the variables' values are unchanged; otherwise everything rebuilds.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vs/core/settings.hpp"
#include "vs/feature/tree.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/shape.hpp"
#include "vs/sketch/sketch.hpp"

namespace vs::feature {

using core::ProductSettings;

enum class FeatureStatus { Ok, Failed, Suppressed };
std::string_view to_string(FeatureStatus status);

struct NamedFace {
  std::string name;
  kernel::Plane plane;  // a point on the face's plane and its outward normal
};

struct FeatureResult {
  Id feature;
  FeatureStatus status = FeatureStatus::Ok;
  // True when this result was carried over from the previous rebuild.
  bool reused = false;
  Diagnostics diagnostics;

  // The body after this feature.
  kernel::Solid body;
  // The largest kernel tolerance in the body after this feature, if it
  // changed the body.
  double max_tolerance = 0.0;

  // What the feature produced, when it succeeded:
  std::optional<sketch::SketchPlane> plane;      // datum planes and sketches
  std::optional<kernel::Axis> axis;              // datum axes (unit direction)
  std::optional<dvec3> point;                    // datum points
  std::shared_ptr<const sketch::Sketch> sketch;  // the placed, solved sketch
  std::vector<NamedFace> faces;                  // extrudes and revolves

  [[nodiscard]] bool ok() const { return status == FeatureStatus::Ok; }
};

struct RebuildResult {
  std::vector<FeatureResult> features;  // one per feature, in tree order
  kernel::Solid body;                   // the final body; empty if nothing made one
  VariableValues variables;
  // The variables' diagnostics, then every feature's, in order.
  Diagnostics diagnostics;

  [[nodiscard]] bool ok() const { return !diagnostics.has_errors(); }
  [[nodiscard]] const FeatureResult* find(const Id& feature) const;
  // How many features ran in this rebuild (not reused, not suppressed).
  [[nodiscard]] std::size_t rebuilt_count() const;
};

struct RebuildOptions {
  // Rebuild from `start` onwards, reusing `previous` for the features above
  // it (see the top of this header). Both or neither.
  const RebuildResult* previous = nullptr;
  Id start;
};

RebuildResult rebuild(const FeatureTree& tree, const ProductSettings& settings,
                      const RebuildOptions& options = {});

// The frame of a principal plane.
sketch::SketchPlane principal_frame(PrincipalPlane plane);

// The frame used for a plane known only by a point and a normal (a face or
// an offset): origin at the projection of the model origin, x along the
// model axis most perpendicular to the normal (X, then Y, then Z on ties),
// projected into the plane.
sketch::SketchPlane frame_for(const kernel::Plane& plane);

}  // namespace vs::feature

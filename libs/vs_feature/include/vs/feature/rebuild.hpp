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
//   feature.sweep.no_path / .path_branches / .path_disconnected
//                                     the path sketch isn't one chain of curves
//   feature.loft.too_few_sections / .section_not_single
//                                     fewer than two sections, or a section that
//                                     isn't one profile without holes
//   feature.shell.no_faces / .bad_thickness, feature.fillet.no_edges /
//   .bad_radius, feature.chamfer.no_edges / .bad_distance
//   feature.pattern.no_features / .not_solid_feature / .bad_count /
//   .too_many / .bad_spacing / .bad_angle
//   feature.cut.no_body               a cut with nothing to cut from
//   kernel.*                          the kernel's own errors, passed through
//
// Fallbacks for the kernel's weak spots. When a fillet, chamfer or shell
// fails in the kernel (kernel.fillet.failed, kernel.chamfer.failed,
// kernel.shell.failed), the rebuild retries with smaller values to find
// roughly the largest one that works, and the message names it ("a radius
// of about 2.4 mm works here"), as does a "feature.fallback.works" info
// diagnostic that follows the error. The kernel also rejects shells too thick for
// a curved face before trying (kernel.shell.too_thick). A failure never
// leaves an invalid solid: the feature fails and the body stays as it was.
//
// After every feature that changes the body the rebuild runs the kernel's
// tolerance audit (decision 0001) and reports entities above the product
// tolerance as warnings on that feature.
//
// Booleans: every feature combines with the body using the product's linear
// tolerance as the fuzzy value, so faces within the tolerance of each other
// count as touching (decision 0002).
//
// Face names. Features name the faces they make, so a sketch or datum can
// sit on them, a shell can open them and a fillet can find edges by them
// (FaceRef):
//
//   Extrude (add)  "start"         the face on the sketch plane (symmetric: the far
//                                  side behind it), facing back along the extrusion
//                  "end"           the far face
//                  "side:<id>"     the side face made by sketch curve <id>: planar
//                                  for a line; cylindrical for a circle or arc,
//                                  found by sampled points for a Bézier (both
//                                  only without draft)
//   Extrude (cut)  "end"           the floor of a blind or up-to-plane cut
//                  "side:<id>"     the wall made by sketch curve <id>
//   Revolve        "start", "end"  the flat ends of a partial revolve
//   Sweep          "start", "end"  the caps of an open path ("end" only when the
//                                  path starts on the profile's plane)
//   Loft           "start", "end"  the first and last sections
//   Patterns       "<k>:<feature id>:<name>"   face <name> of copy k (1 to
//                                  count - 1) of the patterned feature <feature id>
//
// Shells, fillets and chamfers name no faces of their own: references to
// faces of earlier features keep working through them as long as the face
// is still there (a fillet trims a face; it doesn't remove it).
//
// A FaceRef is found again by geometry: the referenced feature reports where
// its named face lies in this rebuild (a plane and outward normal, or a
// cylinder's axis, radius and side), and the reference resolves to the
// faces of the body (as the referring feature sees it) on that surface,
// facing the same way. So references follow parameter changes, and survive
// later features that split or trim the face, but break when the face is
// cut away entirely or its sketch curve is deleted. The spec accepts this
// fragility inside a volume. A sketch or datum needs a planar face; naming a
// cylindrical one is "feature.reference.wrong_kind".
//
// An EdgeRef resolves to the edges the body's faces for `face` share with
// its faces for `other`, or to every edge of the faces for `face`. Edges
// found by several references count once. When the faces no longer meet,
// the reference is broken.
//
// Rebuilding from a changed feature: pass the previous result and the first
// feature that changed. Results above it are reused when every feature above
// it (definitions, sketches, suppression) is what the previous result was
// built from, and the variables' values and the ProductSettings are
// unchanged; otherwise everything rebuilds. A wrong `start` therefore never
// reuses a stale result.

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vs/core/settings.hpp"
#include "vs/feature/tree.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/kernel/shape.hpp"
#include "vs/sketch/sketch.hpp"

namespace vs::feature {

using core::ProductSettings;

enum class FeatureStatus { Ok, Failed, Suppressed };
std::string_view to_string(FeatureStatus status);

// A point on a named face's surface and the face's outward normal there.
struct SurfacePoint {
  dvec3 point;
  dvec3 normal;
};

struct NamedFace {
  std::string name;
  // For a planar face: a point on the face's plane and its outward normal.
  kernel::Plane plane;
  // Set for a cylindrical face, whose `plane` is then meaningless.
  std::optional<kernel::Cylinder> cylinder;
  // Set (not empty) for a face on any other surface, such as the side of an
  // extruded Bézier (decision 0040): points on the surface and the normals
  // there. `plane` is then meaningless. A body face is on it when its
  // surface passes through every point, facing the same way at each.
  std::vector<SurfacePoint> samples;

  // On a cylinder or another curved surface: not a plane.
  [[nodiscard]] bool curved() const { return cylinder.has_value() || !samples.empty(); }
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
  std::vector<NamedFace> faces;                  // solid features and patterns
  // The solid an extrude, revolve, sweep or loft added or cut, and which;
  // patterns copy it.
  kernel::Solid tool;
  std::optional<Operation> operation;

  [[nodiscard]] bool ok() const { return status == FeatureStatus::Ok; }
};

struct RebuildResult {
  std::vector<FeatureResult> features;  // one per feature, in tree order
  kernel::Solid body;                   // the final body; empty if nothing made one
  VariableValues variables;
  // The variables' diagnostics, then every feature's, in order.
  Diagnostics diagnostics;
  // What it was built from, so that a later rebuild reuses it only above
  // features that are unchanged, under the same settings.
  std::vector<Feature> built_from;
  ProductSettings settings;

  [[nodiscard]] bool ok() const { return !diagnostics.has_errors(); }
  [[nodiscard]] const FeatureResult* find(const Id& feature) const;
  // How many features ran in this rebuild (not reused, not suppressed).
  [[nodiscard]] std::size_t rebuilt_count() const;
};

// Inputs from outside the tree, for a tree that lives inside something else
// (vs_model: a parametric volume's contents see their boundary and connection
// surfaces, and the volume's parameters).
//
// Each reference input stands in for a feature that isn't in the tree:
// a FeatureRef or FaceRef to its id resolves to its `result` (its plane,
// axis, point or named faces) as if a feature above everything had made it.
// A sketch or datum on one of its named planar faces takes the face's plane
// directly, without needing a face of the body there; shells, fillets and
// chamfers still find the body's faces on it. An id that is both a feature
// of the tree and an input refers to the feature.
//
// `values` are numbers that Params and sketch dimensions can use by name, as
// if they were variables. A variable of the tree with the same name wins,
// even when it has no value (its expression failed): it is never shadowed.
// The variable table itself doesn't see them: variables can't use them.
struct ExternalInput {
  core::SubjectRef subject;  // its id is the id references use
  FeatureResult result;      // `feature` and `status` are ignored
};
struct ExternalInputs {
  std::vector<ExternalInput> references;
  std::map<std::string, double, std::less<>> values;

  [[nodiscard]] const ExternalInput* find(const Id& id) const;
};

struct RebuildOptions {
  // Rebuild from `start` onwards, reusing `previous` for the features above
  // it (see the top of this header). Both or neither. Nothing is reused when
  // `inputs` has references, because the rebuild can't tell whether they
  // changed.
  const RebuildResult* previous = nullptr;
  Id start;
  const ExternalInputs* inputs = nullptr;
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

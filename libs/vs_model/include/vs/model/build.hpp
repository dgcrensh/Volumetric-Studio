#pragma once

// Building a document's authored geometry: the layout, each volume's
// boundary, each connection surface's patch, and the contents M5 can make
// (parametric trees and imported STEP/IGES files).
//
// build() runs on the calling thread, in the spec's order: rebuild the
// layout, then boundaries (with keep-outs cut away), then connection
// surfaces and volume parameters, then contents. It never throws; a part
// that fails reports why and the rest carries on. build() builds
// everything every time; vs_eval (M9) schedules and caches contents by
// calling the two halves itself: build_layout(), then build_contents() for
// each volume that needs it, possibly on several threads at once.
//
// Boundaries. A feature boundary is the union of its features' tools (the
// solid each adds); a box boundary spans its two points. Keep-outs then cut
// it. Codes, naming the volume's boundary first:
//   model.boundary.feature_failed   a boundary feature failed or is suppressed
//   model.boundary.point_failed     a box corner can't be found
//   model.boundary.degenerate       the box has no volume
//   model.boundary.cut_away         keep-outs leave nothing
//   kernel.*                        passed through
//
// Connection surfaces. A sketch patch is its sketch's one profile (holes
// allowed); a datum patch is its rectangle; a cylinder patch is its part
// of the cylinder (a band for a full turn). The surface's frame is the
// sketch's or plane's frame, or the cylinder's (cylinder_frame()). Bolted
// flanges also get their hole centres.
//   model.connection.sketch_failed  the sketch failed or isn't a sketch
//   model.connection.profiles       the sketch has no profile, or several
//   model.connection.plane_failed   the datum patch's plane can't be found
//   model.connection.axis_failed    the cylinder patch's axis can't be found
//   model.connection.bad_size       a non-positive width or height; a
//                                   cylinder's radius, arc or length below
//                                   the minimum feature size, or a sweep
//                                   over a full turn
//   model.connection.bad_pattern    a hole pattern count isn't a whole number >= 1
//   feature.expression.*, feature.variable.*   a parameter's expression
//
// Contents. A parametric volume's tree is rebuilt with its boundary,
// connection surfaces and parameters as external inputs
// (feature::ExternalInputs):
//
//   the boundary (by the volume's id) names its planar and cylindrical faces:
//     for a feature boundary, "<feature id>:<name>" for each face <name> its
//     features name (rebuild.hpp), so FaceRef{volume id, "<id>:end"} is the
//     top of an extruded boundary; for a box boundary, "-x", "+x", "-y",
//     "+y", "-z", "+z", each facing out;
//   each connection surface joining the volume (by its id) is a plane: its
//     frame (FeatureRef) and its face "patch" (FaceRef); a cylindrical one
//     is its axis (FeatureRef) and its face "patch", a cylinder whose
//     normal points away from the axis;
//   each parameter is a value its expressions can use by name.
//
// Static contents read their blob when the build is given the project's
// blobs (STEP and IGES; mesh files are read by vs_eval's engine, M9, and
// vs_mesh's combine(), M8, and stay Stale here). Procedural
// contents stay Stale here too: vs_proc generates them through vs_eval's
// engine (M10).
//   model.contents.failed           the contents' rebuild has errors
//   model.contents.missing_blob     the blob isn't in the project
//   model.contents.import_failed    the file doesn't read as a solid
//   model.contents.no_boundary      (info) contents not built: the boundary failed
//
// Published outputs are measured on the contents when there are any.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "vs/core/box.hpp"
#include "vs/core/settings.hpp"
#include "vs/feature/rebuild.hpp"
#include "vs/kernel/shape.hpp"
#include "vs/model/document.hpp"
#include "vs/model/project.hpp"

namespace vs::model {

using core::dbox3;
using core::dvec3;
using core::ProductSettings;

// The spec's volume statuses. M5 reports Current for contents it built (or
// that need no building: placeholders), Failed when they failed and Stale
// for contents only a later milestone can generate.
enum class VolumeStatus { Current, Stale, Regenerating, Failed };
std::string_view to_string(VolumeStatus status);

using OutputValue = std::variant<double, dvec3, dbox3>;

struct OutputResult {
  Id output;
  std::optional<OutputValue> value;  // none when there are no contents
};

struct VolumeBuild {
  Id volume;
  VolumeStatus status = VolumeStatus::Stale;
  kernel::Solid boundary;  // after keep-outs; empty when it failed
  // The boundary's named faces (see "Contents" above): the planes and
  // cylinders its faces lie on, by name. Empty when the boundary failed.
  std::vector<feature::NamedFace> boundary_faces;
  std::map<std::string, double, std::less<>> parameters;  // values by name
  std::optional<feature::RebuildResult> contents_rebuild;  // parametric contents
  kernel::Solid contents;  // the contents' solid, when there is one
  std::vector<OutputResult> outputs;
  Diagnostics diagnostics;
};

// A cylindrical patch, evaluated (CylinderPatch). The cylinder's axis is
// the surface frame's origin and normal; angles are measured from its x
// axis, counter-clockwise about the normal.
struct CylinderBuild {
  double radius = 0.0;       // mm
  double start_angle = 0.0;  // radians
  double sweep = 0.0;        // radians, (0, 2π]
  double start = 0.0;        // mm along the axis
  double length = 0.0;       // mm
  // A full turn: a ring round the axis with no ends round it.
  [[nodiscard]] bool band() const;
};

// The frame of a cylinder round `axis`: origin at the axis's origin,
// normal along its direction, and an x axis (angle 0) that depends on the
// direction alone: the principal axis least aligned with it, made
// perpendicular.
sketch::SketchPlane cylinder_frame(const kernel::Axis& axis);

struct ConnectionBuild {
  Id surface;
  bool ok = false;
  kernel::Face patch;
  // A planar patch's plane (normal = frame.normal()), or a cylindrical
  // patch's frame (cylinder_frame() of its axis).
  sketch::SketchPlane frame;
  std::optional<CylinderBuild> cylinder;  // set for a cylindrical patch
  double area = 0.0;          // mm²
  std::vector<dvec3> hole_centres;  // bolted flanges
  std::optional<double> gap;        // clearance: the evaluated gap, mm
  std::optional<double> min_wall;   // bolted flange: the evaluated minimum wall, mm
  Diagnostics diagnostics;
};

struct DocumentBuild {
  feature::RebuildResult layout;
  std::vector<VolumeBuild> volumes;          // in document order
  std::vector<ConnectionBuild> connections;  // in document order
  // The layout's diagnostics, then each volume's, then each surface's.
  Diagnostics diagnostics;

  [[nodiscard]] bool ok() const { return !diagnostics.has_errors(); }
  [[nodiscard]] const VolumeBuild* find_volume(const Id& volume) const;
  [[nodiscard]] const ConnectionBuild* find_connection(const Id& surface) const;
};

// The contents half of a volume's build (see VolumeBuild).
struct ContentsBuild {
  VolumeStatus status = VolumeStatus::Stale;
  std::optional<feature::RebuildResult> contents_rebuild;
  kernel::Solid contents;
  std::vector<OutputResult> outputs;
  Diagnostics diagnostics;
};

// The layout half: the layout, boundaries, connection surfaces and volume
// parameters. No contents are built: each VolumeBuild has no contents and
// no outputs, and its status is Failed when its boundary or a parameter
// failed and Stale otherwise. `diagnostics` collects what was built.
DocumentBuild build_layout(const Document& document, const ProductSettings& settings);

// The contents of `document.volumes[index]`, given a build_layout() result
// of the same document. It only reads `layout`, so calls for different
// volumes may run on different threads at the same time.
ContentsBuild build_contents(const Document& document, const DocumentBuild& layout,
                             std::size_t index, const ProductSettings& settings,
                             const BlobStore* blobs = nullptr);

// Moves `contents` into `volume`: status, contents, outputs, and its
// diagnostics after the volume's own. (The document's `diagnostics` list
// isn't updated.)
void apply_contents(VolumeBuild& volume, ContentsBuild contents);

// `blobs` holds static contents' files; without it static contents aren't
// read and stay Stale.
DocumentBuild build(const Document& document, const ProductSettings& settings,
                    const BlobStore* blobs = nullptr);

}  // namespace vs::model

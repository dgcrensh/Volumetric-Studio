#pragma once

// Building a document's authored geometry: the layout, each volume's
// boundary, each connection surface's patch, and the contents M5 can make
// (parametric trees and imported STEP/IGES files).
//
// build() runs on the calling thread, in the spec's order: rebuild the
// layout, then boundaries (with keep-outs cut away), then connection
// surfaces, then contents. It never throws; a part that fails reports why
// and the rest carries on. Scheduling, caching and background generation
// are M9's; M5 builds everything every time.
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
// allowed); a datum patch is its rectangle. The surface's frame is the
// sketch's or plane's frame. Bolted flanges also get their hole centres.
//   model.connection.sketch_failed  the sketch failed or isn't a sketch
//   model.connection.profiles       the sketch has no profile, or several
//   model.connection.plane_failed   the datum patch's plane can't be found
//   model.connection.bad_size       a non-positive width or height
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
//     frame (FeatureRef) and its face "patch" (FaceRef);
//   each parameter is a value its expressions can use by name.
//
// Static contents read their blob when the build is given the project's
// blobs (STEP and IGES; meshes wait for M8 and stay Stale). Procedural
// contents wait for M10 and stay Stale.
//   model.contents.failed           the contents' rebuild has errors
//   model.contents.missing_blob     the blob isn't in the project
//   model.contents.import_failed    the file doesn't read as a solid
//   model.contents.no_boundary      (info) contents not built: the boundary failed
//
// Published outputs are measured on the contents when there are any.

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
  std::map<std::string, double, std::less<>> parameters;  // values by name
  std::optional<feature::RebuildResult> contents_rebuild;  // parametric contents
  kernel::Solid contents;  // the contents' solid, when there is one
  std::vector<OutputResult> outputs;
  Diagnostics diagnostics;
};

struct ConnectionBuild {
  Id surface;
  bool ok = false;
  kernel::Face patch;
  sketch::SketchPlane frame;  // the patch's plane; normal = frame.normal()
  double area = 0.0;          // mm²
  std::vector<dvec3> hole_centres;  // bolted flanges
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

// `blobs` holds static contents' files; without it static contents aren't
// read and stay Stale.
DocumentBuild build(const Document& document, const ProductSettings& settings,
                    const BlobStore* blobs = nullptr);

}  // namespace vs::model

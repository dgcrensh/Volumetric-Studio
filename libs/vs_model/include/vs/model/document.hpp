#pragma once

// The document: the volume contract in data (spec, "Core concepts").
//
// A Document holds
//
//   - the root layout: a feature tree (with the global variables) whose
//     solid features and datums define boundaries and connection surfaces;
//   - the volumes directly under the root (the MVP has one level of
//     nesting; composite volumes are a later milestone);
//   - the connection surfaces between them, which the root owns because it
//     is the common parent of every pair of volumes (spec: "owner: always
//     the common parent ... never one of those volumes");
//   - keep-out regions, which cut volumes' boundaries before contents
//     generate;
//   - links, through which a volume's published output feeds a layout
//     variable or another volume's parameter.
//
// Everything has a stable Id. Ids are unique across the whole document,
// contents included: an id names one thing, so references can be checked
// (references.hpp). Authoring goes through edit() (check.hpp), which refuses
// a change that breaks encapsulation or closes a dependency loop.
//
// This header is pure data; building boundaries, patches and contents is
// build.hpp, and files are project.hpp.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/feature/tree.hpp"

namespace vs::model {

using core::Diagnostic;
using core::Diagnostics;
using core::Id;
using core::Result;
using core::SubjectRef;
using feature::Param;

// --- boundaries ---------------------------------------------------------------

// The union of the solids that layout features add (extrude, revolve,
// sweep, loft with operation Add, in any order). The features themselves
// still add to the layout body; the boundary takes their tools.
struct FeatureBoundary {
  std::vector<Id> features;
  friend bool operator==(const FeatureBoundary&, const FeatureBoundary&) = default;
};

// The axis-aligned box spanned by two points of the layout (datum points or
// sketch points): reference geometry, without a solid feature.
struct BoxBoundary {
  feature::PointRef min;
  feature::PointRef max;
  friend bool operator==(const BoxBoundary&, const BoxBoundary&) = default;
};

using Boundary = std::variant<FeatureBoundary, BoxBoundary>;

// --- contents -------------------------------------------------------------------

enum class VolumeKind { Static, Parametric, Procedural };
std::string_view to_string(VolumeKind kind);

// Imported or baked geometry, held as a blob of the project (project.hpp).
// An empty `blob` is a placeholder volume: blocked out, not filled yet.
// Baked contents (vs_proc bake.hpp, M12) keep the graph they were baked
// from, opaque here as in ProceduralContents, so they can be unbaked.
struct StaticContents {
  std::string blob;    // the blob's path in the project, e.g. "blobs/<hash>.step"
  std::string format;  // "step", "iges", "stl", "3mf", "obj"
  std::string baked_graph;  // the graph's JSON when baked; empty otherwise
  friend bool operator==(const StaticContents&, const StaticContents&) = default;
};

// The volume's own feature tree. Its sketches and datums may anchor only to
// the volume's boundary and its connection surfaces (references.hpp), and
// its expressions may use the volume's parameters by name. Everything in it
// is private to the volume.
struct ParametricContents {
  feature::FeatureTree tree;
  friend bool operator==(const ParametricContents&, const ParametricContents&) = default;
};

// A node graph: vs_proc's JSON (proc/graph.hpp, M10). vs_model sits below
// vs_proc, so here it is opaque text, stored and hashed as it is; vs_proc
// checks it against the document when the engine generates the volume.
struct ProceduralContents {
  std::string graph;  // JSON; empty: no graph yet
  friend bool operator==(const ProceduralContents&, const ProceduralContents&) = default;
};

using Contents = std::variant<StaticContents, ParametricContents, ProceduralContents>;

// --- parameters and published outputs -----------------------------------------------

// A number the volume exposes to its contents. The expression may use the
// layout's variables; a link (below) can drive it from a published output.
struct VolumeParameter {
  Id id;
  std::string name;  // a legal variable name (feature::is_valid_name)
  Param value;
  friend bool operator==(const VolumeParameter&, const VolumeParameter&) = default;
};

enum class OutputQuantity {
  Volume,          // mm³ of the contents (scalar)
  Mass,            // g, from the volume and `density` in g/cm³ (scalar)
  SurfaceArea,     // mm² (scalar)
  CentreOfGravity, // mm (vector)
  Envelope,        // the contents' bounding box (box)
  Published,       // a number the procedure publishes under the output's name
                   // (counts, say: vs_proc's output.publish node, M12) (scalar)
};
std::string_view to_string(OutputQuantity quantity);
// Whether the quantity is one number, so it can feed a variable or parameter.
bool is_scalar(OutputQuantity quantity);

// A read-only value a volume exposes (spec, "Published outputs"). Its value
// comes from the contents; M5 computes it for parametric contents.
struct PublishedOutput {
  Id id;
  std::string name;
  OutputQuantity quantity = OutputQuantity::Volume;
  double density = 1.0;  // g/cm³, Mass only
  friend bool operator==(const PublishedOutput&, const PublishedOutput&) = default;
};

// --- volumes ------------------------------------------------------------------------

struct Volume {
  Id id;
  std::string name;
  Boundary boundary;
  // A keep-out volume reserves its boundary as empty space for its siblings:
  // it never has contents (its contents must be an empty placeholder), and
  // keep-out regions may use its boundary as their cutter.
  bool keep_out = false;
  Contents contents;
  std::vector<VolumeParameter> parameters;
  std::uint64_t seed = 0;
  std::vector<PublishedOutput> outputs;

  [[nodiscard]] VolumeKind kind() const { return static_cast<VolumeKind>(contents.index()); }
  // True for a placeholder: no file, no features, no graph.
  [[nodiscard]] bool contents_empty() const;
  [[nodiscard]] const feature::FeatureTree* tree() const;
  [[nodiscard]] feature::FeatureTree* tree();
  [[nodiscard]] const VolumeParameter* find_parameter(const Id& id) const;
  [[nodiscard]] const VolumeParameter* find_parameter(std::string_view name) const;
  [[nodiscard]] const PublishedOutput* find_output(const Id& id) const;
  [[nodiscard]] const PublishedOutput* find_output(std::string_view name) const;

  friend bool operator==(const Volume&, const Volume&) = default;
};

// --- keep-out regions ---------------------------------------------------------------

// An authored cut that subtracts from volumes' boundaries before their
// contents generate (spec: "Cutting through a volume"). Each cutter is a
// layout solid feature (its tool, as for FeatureBoundary) or a keep-out
// volume (its boundary).
struct KeepOut {
  Id id;
  std::string name;
  std::vector<Id> cutters;
  std::vector<Id> volumes;  // the volumes it cuts
  friend bool operator==(const KeepOut&, const KeepOut&) = default;
};

// --- connection surfaces -------------------------------------------------------------

// The patch is the closed profile of a layout sketch: one outer loop,
// holes allowed, on the sketch's plane.
struct SketchPatch {
  Id sketch;
  friend bool operator==(const SketchPatch&, const SketchPatch&) = default;
};

// A rectangle on a plane of the layout (a datum plane, sketch plane or a
// layout face), centred at (u, v) in that plane's frame, `width` along its
// x axis and `height` along its y axis.
struct DatumPatch {
  feature::PlaneRef plane = feature::PrincipalPlane::XY;
  Param u, v;
  Param width = Param::of(10.0);
  Param height = Param::of(10.0);
  friend bool operator==(const DatumPatch&, const DatumPatch&) = default;
};

using PatchGeometry = std::variant<SketchPatch, DatumPatch>;

enum class ConnectionType { Bonded, Clearance, BoltedFlange };
std::string_view to_string(ConnectionType type);

// Faces fused together: no parameters.
struct BondedParams {
  friend bool operator==(const BondedParams&, const BondedParams&) = default;
};

// The volumes stay apart by `gap` across the surface.
struct ClearanceParams {
  Param gap = Param::of(0.2);
  friend bool operator==(const ClearanceParams&, const ClearanceParams&) = default;
};

// Where bolt holes go, in the patch plane's frame about (u, v).
struct BoltCircle {
  Param count = Param::of(4);
  Param diameter = Param::of(30.0);  // pitch circle diameter
  Param start_angle;                 // radians, from the frame's x axis
  friend bool operator==(const BoltCircle&, const BoltCircle&) = default;
};
struct BoltGrid {
  Param columns = Param::of(2);
  Param rows = Param::of(1);
  Param pitch_x = Param::of(20.0);
  Param pitch_y = Param::of(20.0);
  friend bool operator==(const BoltGrid&, const BoltGrid&) = default;
};
struct HolePattern {
  std::variant<BoltCircle, BoltGrid> layout;
  Param u, v;  // the pattern's centre in the patch plane's frame
  friend bool operator==(const HolePattern&, const HolePattern&) = default;
};

// Two volumes bolted across the surface. `thread` is an ISO metric coarse
// size ("M2" to "M12", see thread_size()); `min_wall` is the least material
// required round each hole (checked against the pattern in M6).
struct BoltedFlangeParams {
  HolePattern holes;
  std::string thread = "M4";
  Param min_wall = Param::of(2.0);
  friend bool operator==(const BoltedFlangeParams&, const BoltedFlangeParams&) = default;
};

using ConnectionParams = std::variant<BondedParams, ClearanceParams, BoltedFlangeParams>;

// ISO metric coarse threads: nominal diameter and medium-fit clearance hole
// (ISO 273), mm.
struct ThreadSize {
  std::string_view name;
  double nominal = 0;
  double clearance_hole = 0;
};
std::optional<ThreadSize> thread_size(std::string_view name);
std::vector<ThreadSize> thread_sizes();

// How the seam along the surface's outline is divided (spec, "Seam
// conformity"). vs_mesh (seam_library.hpp, M7) derives the division from
// the patch: `spacing` 0 means "derive automatically" (a sixteenth of the
// outline's diagonal); a positive value, in mm, overrides it.
struct SeamDefinition {
  double spacing = 0.0;  // mm
  friend bool operator==(const SeamDefinition&, const SeamDefinition&) = default;
};

// A typed, named patch where volumes meet (spec, "Connection surfaces as
// named objects"). It joins two sibling volumes, or one volume and the
// outside. The patch is built from layout sketches or datums, never picked
// from result faces, so it stays stable when features above it change.
struct ConnectionSurface {
  Id id;
  std::string name;
  PatchGeometry geometry;
  std::vector<Id> volumes;  // one or two distinct volumes
  ConnectionParams params;
  SeamDefinition seam;

  [[nodiscard]] ConnectionType type() const {
    return static_cast<ConnectionType>(params.index());
  }
  [[nodiscard]] bool joins(const Id& volume) const;
  friend bool operator==(const ConnectionSurface&, const ConnectionSurface&) = default;
};

// --- links ---------------------------------------------------------------------------

// Reference types that may cross a volume boundary (spec, design rule 1).
// These, and nothing else, name something of a volume from outside it.
struct BoundaryRef {
  Id volume;
  friend bool operator==(const BoundaryRef&, const BoundaryRef&) = default;
};
struct ConnectionSurfaceRef {
  Id surface;
  friend bool operator==(const ConnectionSurfaceRef&, const ConnectionSurfaceRef&) = default;
};
struct PublishedOutputRef {
  Id volume;
  Id output;
  friend bool operator==(const PublishedOutputRef&, const PublishedOutputRef&) = default;
};

// A published output feeding a layout variable or a volume parameter (by
// id). The target takes the output's value once the volume has generated
// (M9); until then its own expression stands in. The output must be scalar
// and must not be upstream of its own volume (design rule 6).
struct Link {
  Id id;
  PublishedOutputRef source;
  Id target;
  friend bool operator==(const Link&, const Link&) = default;
};

// --- the document --------------------------------------------------------------------

struct Document {
  Id id;
  std::string name;
  feature::FeatureTree layout;
  std::vector<Volume> volumes;
  std::vector<ConnectionSurface> connections;
  std::vector<KeepOut> keep_outs;
  std::vector<Link> links;

  [[nodiscard]] const Volume* find_volume(const Id& id) const;
  [[nodiscard]] Volume* find_volume(const Id& id);
  [[nodiscard]] const Volume* find_volume(std::string_view name) const;
  [[nodiscard]] const ConnectionSurface* find_connection(const Id& id) const;
  [[nodiscard]] ConnectionSurface* find_connection(const Id& id);
  [[nodiscard]] const KeepOut* find_keep_out(const Id& id) const;
  [[nodiscard]] const Link* find_link(const Id& id) const;

  // The volume whose contents hold `id` (a feature, variable or sketch
  // entity of a parametric volume's tree), if any.
  [[nodiscard]] const Volume* contents_owner(const Id& id) const;

  // Subjects for diagnostics, with readable labels.
  [[nodiscard]] SubjectRef volume_subject(const Id& volume) const;
  [[nodiscard]] SubjectRef boundary_subject(const Id& volume) const;
  [[nodiscard]] SubjectRef connection_subject(const Id& surface) const;
  [[nodiscard]] SubjectRef keep_out_subject(const Id& keep_out) const;
  [[nodiscard]] SubjectRef output_subject(const PublishedOutputRef& output) const;
  [[nodiscard]] SubjectRef link_subject(const Id& link) const;

  friend bool operator==(const Document&, const Document&) = default;
};

// A new, empty document with a random id.
Document make_document(std::string name);

}  // namespace vs::model

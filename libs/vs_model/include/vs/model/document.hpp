#pragma once

// The document: the volume contract in data (spec, "Core concepts").
//
// A Document holds
//
//   - the root layout: a feature tree (with the global variables) whose
//     solid features and datums define boundaries and connection surfaces;
//   - the volumes, at every depth (L3): those directly under the root, and
//     the children of composite volumes, each naming its parent. They are
//     one flat list; the tree is in the `parent` ids (hierarchy.hpp);
//   - the connection surfaces between them, each owned by the common parent
//     of the volumes it joins (spec: "owner: always the common parent ...
//     never one of those volumes"): the root, or a composite volume from
//     whose layout its geometry is built. Keep-outs and links likewise
//     belong to the level of the volumes they name;
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
#include "vs/core/vec.hpp"
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

// --- contents -------------------------------------------------------------------

enum class VolumeKind { Static, Parametric, Procedural, Composite };
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

// What a composite's generated children are filled from (L4): contents,
// parameters and published outputs, as a volume's. A parametric template's
// tree refers to its boundary by the template's own id (faces "-x".."+z",
// as a box volume's tree does); expand() (generated.hpp) gives each copy
// ids derived from the child's, the template's own id becoming the
// child's. A template can't be a composite.
using TemplateContents = std::variant<StaticContents, ParametricContents, ProceduralContents>;

struct ChildTemplate {
  Id id;
  std::string name;
  TemplateContents contents;
  std::vector<VolumeParameter> parameters;
  std::vector<PublishedOutput> outputs;
  // The min corner of the box a parametric template was authored for:
  // a copy in a box with min corner m is built offset by m - origin.
  core::dvec3 origin{};
  friend bool operator==(const ChildTemplate&, const ChildTemplate&) = default;
};

// Which child meets a connection surface that joins a composite volume from
// outside (L3). The surface is a fixed input of the composite, which passes
// it down to the child lying against it; the child takes it as a fixed
// input too, and passes it further down if it is a composite itself.
struct Delegation {
  Id surface;
  Id child;
  friend bool operator==(const Delegation&, const Delegation&) = default;
};

// Child volumes (L3). The children are the volumes whose `parent` is this
// one. What the composite holds is its own layout: a feature tree rebuilt
// as a parametric volume's tree is (its boundary, the connection surfaces
// joining it and its parameters are its inputs), from which its children's
// boundaries, the connection surfaces and keep-outs between them and their
// parameters' expressions are built. It is to its children what the root
// layout is to the top-level volumes.
//
// A composite may also generate children (L4): `generator` is a procedural
// graph (vs_proc JSON, opaque here as in ProceduralContents) whose output
// is a generated layout (generated.hpp), and `templates` are what the
// generated children are filled from. Generated children exist only in an
// evaluated document (vs_eval), never in an authored one; they are
// siblings of the composite's authored children.
struct CompositeContents {
  feature::FeatureTree layout;
  std::vector<Delegation> delegations;
  std::string generator;                 // L4: a graph's JSON; empty: none
  std::vector<ChildTemplate> templates;  // L4
  [[nodiscard]] const Delegation* delegation(const Id& surface) const;
  [[nodiscard]] const ChildTemplate* find_template(std::string_view name) const;
  [[nodiscard]] bool generates() const { return !generator.empty(); }
  friend bool operator==(const CompositeContents&, const CompositeContents&) = default;
};

using Contents =
    std::variant<StaticContents, ParametricContents, ProceduralContents, CompositeContents>;

// A template's contents as a volume's, and back (null for a composite).
Contents as_contents(const TemplateContents& contents);
std::optional<TemplateContents> as_template_contents(const Contents& contents);

// --- volumes ------------------------------------------------------------------------

struct Volume {
  Id id;
  std::string name;
  // The composite volume this one is a child of; null directly under the
  // root (L3). Its boundary, and the surfaces, keep-outs and links naming
  // it, are built from that parent's layout.
  Id parent;
  Boundary boundary;
  // A keep-out volume reserves its boundary as empty space for its siblings:
  // it never has contents (its contents must be an empty placeholder), and
  // keep-out regions may use its boundary as their cutter.
  bool keep_out = false;
  Contents contents;
  // Made by its composite parent's generator (L4): only ever in an
  // evaluated document. Its id derives from the parent's and the
  // generator's key, and nothing authored can name it.
  bool generated = false;
  // Where its parametric contents were authored, relative to where they are
  // built (L4): the tree is rebuilt with every input moved by -offset, and
  // the result moved by +offset, so a template authored for one box fills
  // any box of the same size. Zero (the default) for authored volumes.
  core::dvec3 contents_offset{};
  std::vector<VolumeParameter> parameters;
  std::uint64_t seed = 0;
  std::vector<PublishedOutput> outputs;

  [[nodiscard]] VolumeKind kind() const { return static_cast<VolumeKind>(contents.index()); }
  // True for a placeholder: no file, no features, no graph. A composite is
  // never a placeholder.
  [[nodiscard]] bool contents_empty() const;
  [[nodiscard]] bool composite() const {
    return std::holds_alternative<CompositeContents>(contents);
  }
  // The feature tree inside the volume: a parametric volume's tree, or a
  // composite's layout (L3). Both are private to the volume and rebuilt
  // with its inputs.
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

// Patches are planar (a sketch profile or a datum rectangle) or, since L2,
// cylindrical (CylinderPatch).
//
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

// A curved patch (L2): part of the cylinder of `radius` round a layout
// axis. Round the axis it runs from `start_angle` through
// `start_angle + sweep` (radians, counter-clockwise about the axis's
// direction, from angle 0: build.hpp's cylinder_frame()); along it, from
// `start` to `start + length` (mm from the axis's origin). A sweep of a
// full turn (2π) makes a band: a ring round the axis, as a shaft and its
// bore share. Bolted flanges can't be curved (check.hpp).
struct CylinderPatch {
  feature::AxisRef axis = feature::PrincipalAxis::Z;
  Param radius = Param::of(10.0);
  Param start_angle;
  Param sweep = Param::of(6.283185307179586);  // a full turn: a band
  Param start;
  Param length = Param::of(10.0);
  friend bool operator==(const CylinderPatch&, const CylinderPatch&) = default;
};

using PatchGeometry = std::variant<SketchPatch, DatumPatch, CylinderPatch>;

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

// --- configurations (L5) ---------------------------------------------------------------

// One number a configuration drives: `target` is a variable of the root's
// or a composite's layout, or a volume's parameter (what a link may drive,
// at any depth); `expression` is what it takes there, evaluated where the
// target is (configuration.hpp).
struct ConfigurationValue {
  Id target;
  std::string expression;
  friend bool operator==(const ConfigurationValue&, const ConfigurationValue&) = default;
};

// A named variant of the document (spec, "Later": configurations and
// design tables): values for some of the numbers fixed inputs are made
// of. Every number it doesn't drive keeps its authored value. A design
// table (design_table.hpp) is every configuration at once.
struct Configuration {
  Id id;
  std::string name;
  std::vector<ConfigurationValue> values;  // one per target at most
  [[nodiscard]] const ConfigurationValue* find(const Id& target) const;
  friend bool operator==(const Configuration&, const Configuration&) = default;
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
  // L5: the document's configurations, and the one it builds (null: the
  // authored values). Both written to JSON only when set.
  std::vector<Configuration> configurations;
  Id active_configuration;

  [[nodiscard]] const Volume* find_volume(const Id& id) const;
  [[nodiscard]] Volume* find_volume(const Id& id);
  [[nodiscard]] const Volume* find_volume(std::string_view name) const;
  [[nodiscard]] const ConnectionSurface* find_connection(const Id& id) const;
  [[nodiscard]] ConnectionSurface* find_connection(const Id& id);
  [[nodiscard]] const KeepOut* find_keep_out(const Id& id) const;
  [[nodiscard]] const Link* find_link(const Id& id) const;
  [[nodiscard]] const Configuration* find_configuration(const Id& id) const;
  [[nodiscard]] const Configuration* find_configuration(std::string_view name) const;

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
  [[nodiscard]] SubjectRef configuration_subject(const Id& configuration) const;

  friend bool operator==(const Document&, const Document&) = default;
};

// A new, empty document with a random id.
Document make_document(std::string name);

}  // namespace vs::model

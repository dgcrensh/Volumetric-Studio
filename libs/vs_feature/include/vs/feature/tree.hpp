#pragma once

// The feature tree: an ordered history of sketches, reference geometry and
// solid features, plus the global variables that drive them.
//
// Features run top to bottom; each sees the results of those above it and
// never those below. A feature is referred to by its Id, so renaming or
// reordering doesn't break references (moving a feature above something it
// uses does; the rebuild reports it).
//
// Kinds of feature:
//
//   Sketch       a vs_sketch Sketch placed on a plane (a principal plane, a
//                datum plane, another sketch's plane or a planar face of the
//                body). Dimensions with an expression take their value from
//                the variables before the sketch is solved.
//   DatumPlane   offset from a plane; through three points; or through an
//                axis at an angle to a plane.
//   DatumAxis    through two points; where two planes meet; or along an
//                existing axis (a principal axis or a sketch line).
//   DatumPoint   at coordinates; at an existing point (a sketch point); or
//                where an axis meets a plane.
//   Extrude      a sketch's profiles pushed along its normal, adding material
//                or cutting it (an "extruded cut"). Ends: blind, through all,
//                up to a plane, or symmetric about the sketch; optional draft.
//   Revolve      a sketch's profiles turned about an axis, adding or cutting.
//   Sweep        a sketch's profiles swept along a path drawn in another sketch.
//   Loft         a solid through the profiles of two or more sketches.
//   Shell        the body hollowed to a wall thickness, with faces opened.
//   Fillet       edges of the body rounded with a radius.
//   Chamfer      edges of the body bevelled by a distance.
//   Linear / circular pattern   copies of solid features along a direction
//                or about an axis.
//
// Numbers that a user can drive (depths, angles, offsets) are `Param`s: a
// value and an optional expression over the variables, which the rebuild
// evaluates into the value. Lengths are millimetres and angles radians.
//
// References to result faces (FaceRef) name a face by the feature that made
// it and a name that feature gives it; see rebuild.hpp for the names and how
// they are found again after a rebuild.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/feature/variables.hpp"
#include "vs/sketch/sketch.hpp"

namespace vs::feature {

using core::dvec3;

// A number that may be driven by an expression.
struct Param {
  double value = 0.0;
  std::string expression;  // empty: `value` is used as it is

  static Param of(double v) { return {v, {}}; }
  static Param expr(std::string e) { return {0.0, std::move(e)}; }
  friend bool operator==(const Param&, const Param&) = default;
};

// --- references -------------------------------------------------------------

// The three principal planes, with fixed in-plane axes:
//   XY: x = +X, y = +Y, normal +Z
//   YZ: x = +Y, y = +Z, normal +X
//   ZX: x = +Z, y = +X, normal +Y
enum class PrincipalPlane { XY, YZ, ZX };
// The principal axes, through the origin along +X, +Y or +Z.
enum class PrincipalAxis { X, Y, Z };

std::string_view to_string(PrincipalPlane plane);
std::string_view to_string(PrincipalAxis axis);

// Another feature's main output: a datum's plane, axis or point, or a
// sketch's plane.
struct FeatureRef {
  Id feature;
  friend bool operator==(const FeatureRef&, const FeatureRef&) = default;
};

// An entity inside a sketch feature: a line used as an axis, or a point.
struct SketchEntityRef {
  Id sketch;  // the sketch feature's id
  Id entity;  // the entity's id in that sketch
  friend bool operator==(const SketchEntityRef&, const SketchEntityRef&) = default;
};

// A face a feature made, by the name that feature gives it (rebuild.hpp).
struct FaceRef {
  Id feature;
  std::string name;
  friend bool operator==(const FaceRef&, const FaceRef&) = default;
};

using PlaneRef = std::variant<PrincipalPlane, FeatureRef, FaceRef>;
using AxisRef = std::variant<PrincipalAxis, FeatureRef, SketchEntityRef>;
using PointRef = std::variant<FeatureRef, SketchEntityRef>;

// --- features ---------------------------------------------------------------

struct SketchFeature {
  sketch::Sketch sketch;
  // Where the sketch lies. Its SketchPlane is set from this on every rebuild.
  PlaneRef plane = PrincipalPlane::XY;
  friend bool operator==(const SketchFeature&, const SketchFeature&) = default;
};

struct PlaneOffset {
  PlaneRef base = PrincipalPlane::XY;
  Param distance;  // along the base plane's normal
  friend bool operator==(const PlaneOffset&, const PlaneOffset&) = default;
};
// Origin at `a`, x axis towards `b`, normal by the right-hand rule a→b→c.
struct PlaneThroughPoints {
  PointRef a, b, c;
  friend bool operator==(const PlaneThroughPoints&, const PlaneThroughPoints&) = default;
};
// The plane containing `axis` whose normal is the base plane's normal turned
// about the axis by `angle` (right-hand rule). The axis must not be
// perpendicular to the base plane. Origin on the axis; x along the axis.
struct PlaneAxisAngle {
  AxisRef axis = PrincipalAxis::X;
  PlaneRef base = PrincipalPlane::XY;
  Param angle;
  friend bool operator==(const PlaneAxisAngle&, const PlaneAxisAngle&) = default;
};
struct DatumPlaneFeature {
  std::variant<PlaneOffset, PlaneThroughPoints, PlaneAxisAngle> definition;
  friend bool operator==(const DatumPlaneFeature&, const DatumPlaneFeature&) = default;
};

// From `a` towards `b`.
struct AxisThroughPoints {
  PointRef a, b;
  friend bool operator==(const AxisThroughPoints&, const AxisThroughPoints&) = default;
};
// The line where two planes meet, directed along cross(normal a, normal b).
struct AxisPlaneIntersection {
  PlaneRef a = PrincipalPlane::XY;
  PlaneRef b = PrincipalPlane::YZ;
  friend bool operator==(const AxisPlaneIntersection&, const AxisPlaneIntersection&) = default;
};
// An existing axis, such as a sketch line, made into a datum.
struct AxisAlong {
  AxisRef axis = PrincipalAxis::Z;
  friend bool operator==(const AxisAlong&, const AxisAlong&) = default;
};
struct DatumAxisFeature {
  std::variant<AxisThroughPoints, AxisPlaneIntersection, AxisAlong> definition;
  friend bool operator==(const DatumAxisFeature&, const DatumAxisFeature&) = default;
};

struct PointAt {
  Param x, y, z;
  friend bool operator==(const PointAt&, const PointAt&) = default;
};
struct PointAtRef {
  PointRef point;
  friend bool operator==(const PointAtRef&, const PointAtRef&) = default;
};
struct PointAxisPlane {
  AxisRef axis = PrincipalAxis::Z;
  PlaneRef plane = PrincipalPlane::XY;
  friend bool operator==(const PointAxisPlane&, const PointAxisPlane&) = default;
};
struct DatumPointFeature {
  std::variant<PointAt, PointAtRef, PointAxisPlane> definition;
  friend bool operator==(const DatumPointFeature&, const DatumPointFeature&) = default;
};

// Whether a solid feature adds material or removes it.
enum class Operation { Add, Cut };
std::string_view to_string(Operation op);

// How far an extrusion goes.
enum class Extent {
  Blind,       // `depth` from the sketch plane
  ThroughAll,  // past everything the body has in that direction
  UpToPlane,   // to `up_to`, which must be parallel to the sketch plane
  Symmetric,   // `depth` in total, half on each side of the sketch plane
};
std::string_view to_string(Extent extent);

// Extrudes every closed profile of a sketch. It goes along the sketch
// plane's normal when adding and against it when cutting (into the material
// below a sketch on a face); `reverse` flips that. A positive `draft` tapers
// the profile inwards along the extrusion (holes widen).
struct ExtrudeFeature {
  Id sketch;  // a sketch feature above this one
  Operation operation = Operation::Add;
  Extent extent = Extent::Blind;
  Param depth = Param::of(10.0);  // Blind and Symmetric
  PlaneRef up_to = PrincipalPlane::XY;  // UpToPlane
  Param draft;                    // radians
  bool reverse = false;
  friend bool operator==(const ExtrudeFeature&, const ExtrudeFeature&) = default;
};

// Revolves every closed profile of a sketch about `axis` by `angle` (above 0,
// at most a full turn; right-hand rule, `reverse` turns the other way). The
// axis must lie in the sketch plane and not cross a profile.
struct RevolveFeature {
  Id sketch;
  Operation operation = Operation::Add;
  AxisRef axis = PrincipalAxis::Y;
  Param angle = Param::of(6.283185307179586);
  bool reverse = false;
  friend bool operator==(const RevolveFeature&, const RevolveFeature&) = default;
};

// How a swept profile turns as it travels along the path.
enum class SweepOrientation {
  FollowPath,  // keeps its angle to the path (it turns with the bends)
  Fixed,       // keeps its orientation in space; only its position follows
};
std::string_view to_string(SweepOrientation orientation);

// Sweeps every closed profile of `profile` along the path drawn in `path`:
// that sketch's non-construction lines and arcs must join end to end into
// one chain, open or closed (a lone circle is a closed path). An open path
// runs from the end nearer the profile; the profile should sit across the
// path's start.
struct SweepFeature {
  Id profile;  // a sketch feature
  Id path;     // another sketch feature
  Operation operation = Operation::Add;
  SweepOrientation orientation = SweepOrientation::FollowPath;
  friend bool operator==(const SweepFeature&, const SweepFeature&) = default;
};

// A solid through the profiles of two or more sketches, in order. Each
// section sketch must hold exactly one closed profile without holes.
// `ruled` joins neighbouring sections with straight lines.
struct LoftFeature {
  std::vector<Id> sections;
  Operation operation = Operation::Add;
  bool ruled = false;
  friend bool operator==(const LoftFeature&, const LoftFeature&) = default;
};

// Hollows the body into walls `thickness` thick, removing the `open` faces.
// Walls grow inwards from the outer surface, or outwards with `outward`.
struct ShellFeature {
  std::vector<FaceRef> open;
  Param thickness = Param::of(2.0);
  bool outward = false;
  friend bool operator==(const ShellFeature&, const ShellFeature&) = default;
};

// Edges of the body, found by the faces they bound (see rebuild.hpp): the
// edges where `face` meets `other`, or with no `other` every edge of `face`.
struct EdgeRef {
  FaceRef face;
  std::optional<FaceRef> other;
  friend bool operator==(const EdgeRef&, const EdgeRef&) = default;
};

struct FilletFeature {
  std::vector<EdgeRef> edges;
  Param radius = Param::of(1.0);
  friend bool operator==(const FilletFeature&, const FilletFeature&) = default;
};

// An equal-distance chamfer: cuts back `distance` along both faces.
struct ChamferFeature {
  std::vector<EdgeRef> edges;
  Param distance = Param::of(1.0);
  friend bool operator==(const ChamferFeature&, const ChamferFeature&) = default;
};

// Copies of features that add or cut material (extrude, revolve, sweep,
// loft), each applied to the body as the original was. `count` includes the
// original, so 3 makes two copies; it must evaluate to a whole number.
//
// Linear: copy k moves k * spacing along `direction` (`reverse` flips it).
struct LinearPatternFeature {
  std::vector<Id> features;
  AxisRef direction = PrincipalAxis::X;
  Param count = Param::of(2);
  Param spacing = Param::of(10.0);
  bool reverse = false;
  friend bool operator==(const LinearPatternFeature&, const LinearPatternFeature&) = default;
};

// Circular: copies turn about `axis` (right-hand rule; `reverse` turns the
// other way). With `angle` a full turn they are spread evenly around it;
// with less, the last copy lands at `angle`.
struct CircularPatternFeature {
  std::vector<Id> features;
  AxisRef axis = PrincipalAxis::Z;
  Param count = Param::of(4);
  Param angle = Param::of(6.283185307179586);
  bool reverse = false;
  friend bool operator==(const CircularPatternFeature&, const CircularPatternFeature&) = default;
};

using FeatureDefinition =
    std::variant<SketchFeature, DatumPlaneFeature, DatumAxisFeature, DatumPointFeature,
                 ExtrudeFeature, RevolveFeature, SweepFeature, LoftFeature, ShellFeature,
                 FilletFeature, ChamferFeature, LinearPatternFeature, CircularPatternFeature>;

// In FeatureDefinition order.
enum class FeatureKind {
  Sketch,
  DatumPlane,
  DatumAxis,
  DatumPoint,
  Extrude,
  Revolve,
  Sweep,
  Loft,
  Shell,
  Fillet,
  Chamfer,
  LinearPattern,
  CircularPattern,
};
std::string_view to_string(FeatureKind kind);

struct Feature {
  Id id;
  std::string name;  // optional; messages fall back to "<Kind> <n>"
  bool suppressed = false;
  FeatureDefinition definition;

  [[nodiscard]] FeatureKind kind() const;
  template <typename T>
  [[nodiscard]] const T* as() const { return std::get_if<T>(&definition); }
  template <typename T>
  [[nodiscard]] T* as() { return std::get_if<T>(&definition); }
  friend bool operator==(const Feature&, const Feature&) = default;
};

// --- the tree -----------------------------------------------------------------

class FeatureTree {
 public:
  [[nodiscard]] VariableTable& variables() { return variables_; }
  [[nodiscard]] const VariableTable& variables() const { return variables_; }

  // Appends a feature and returns its id. A null id becomes a random one,
  // except for a sketch, which takes its sketch's id.
  Id add(FeatureDefinition definition, std::string name = {}, Id id = {});
  // Adds a fully formed feature at `index` (clamped to the end).
  Id insert(std::size_t index, Feature feature);

  // Removes a feature. Features that used it report a broken reference on
  // the next rebuild.
  bool remove(const Id& id);
  // Moves a feature to `index` (clamped to the end).
  bool move(const Id& id, std::size_t index);
  bool set_suppressed(const Id& id, bool suppressed);

  [[nodiscard]] const std::vector<Feature>& features() const { return features_; }
  [[nodiscard]] const Feature* find(const Id& id) const;
  [[nodiscard]] Feature* find(const Id& id);
  [[nodiscard]] std::optional<std::size_t> index_of(const Id& id) const;

  // The sketch of a sketch feature, for editing; nullptr for anything else.
  [[nodiscard]] sketch::Sketch* sketch(const Id& feature);
  [[nodiscard]] const sketch::Sketch* sketch(const Id& feature) const;

  // The name if set, else a sketch's own name, else "<Kind> <n>" counting
  // features of that kind, such as "Extrude 2".
  [[nodiscard]] std::string label(const Id& id) const;
  [[nodiscard]] core::SubjectRef subject(const Id& id) const;

  // Structural problems: duplicate feature ids ("feature.tree.duplicate_id")
  // and the variable table's own check().
  [[nodiscard]] Diagnostics validate() const;

  friend bool operator==(const FeatureTree&, const FeatureTree&) = default;

 private:
  VariableTable variables_;
  std::vector<Feature> features_;
};

}  // namespace vs::feature

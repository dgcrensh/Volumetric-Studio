#pragma once

// The sketch data model: 2D geometry on a plane, and the constraints that
// relate it.
//
// A sketch holds entities and constraints, each with a stable Id (vs_core).
// Entities are points, lines, circles and arcs. Points are entities in their
// own right: a line refers to its two end points and an arc to its centre,
// start and end points, so two curves that share a point id are joined
// without a coincident constraint (a coincident constraint between two
// distinct points works too).
//
// Positions are in sketch coordinates (millimetres, FP64). `SketchPlane`
// places the sketch in model space. The solver (solver.hpp) moves points to
// satisfy the constraints; nothing in this header solves anything.
//
// Invariants a well-formed sketch keeps, checked by `validate()`:
//   - every id is unique across the sketch's entities and constraints;
//   - every reference names an entity of the right kind (a line's ends are
//     points, a distance's references fit one of its documented forms, ...);
//   - circle radii are finite and positive.
// Editing calls don't enforce these, so a sketch read from a file or edited
// by hand can be checked once, with diagnostics naming what is wrong.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/core/vec.hpp"

namespace vs::sketch {

using core::Diagnostic;
using core::Diagnostics;
using core::dvec2;
using core::dvec3;
using core::Id;
using core::Result;

// --- placement ----------------------------------------------------------

// The plane a sketch lies on: model point = origin + x * x_axis + y * y_axis.
// The axes are unit length and perpendicular; the plane's normal is
// cross(x_axis, y_axis), so loops that run counter-clockwise in the sketch
// face along the normal.
struct SketchPlane {
  dvec3 origin{0, 0, 0};
  dvec3 x_axis{1, 0, 0};
  dvec3 y_axis{0, 1, 0};

  static constexpr SketchPlane xy() { return {}; }

  [[nodiscard]] dvec3 normal() const;
  [[nodiscard]] dvec3 to_model(const dvec2& p) const;

  friend bool operator==(const SketchPlane&, const SketchPlane&) = default;
};

// --- entities -----------------------------------------------------------

enum class EntityKind { Point, Line, Circle, Arc };

std::string_view to_string(EntityKind kind);

struct Point {
  dvec2 position{};
  friend bool operator==(const Point&, const Point&) = default;
};

// A straight segment between two points.
struct Line {
  Id start;
  Id end;
  friend bool operator==(const Line&, const Line&) = default;
};

// A full circle. Its radius is a solver unknown like the centre's position.
struct Circle {
  Id center;
  double radius = 1.0;
  friend bool operator==(const Circle&, const Circle&) = default;
};

// A circular arc running counter-clockwise from `start` to `end` about
// `center`. Its radius is the distance from the centre to the start point;
// the solver keeps the end point on the same radius.
struct Arc {
  Id center;
  Id start;
  Id end;
  friend bool operator==(const Arc&, const Arc&) = default;
};

using Geometry = std::variant<Point, Line, Circle, Arc>;

struct Entity {
  Id id;
  Geometry geometry;
  // Construction geometry guides other geometry and is never part of a
  // profile.
  bool construction = false;
  // Optional user-facing name; messages fall back to a generated label.
  std::string name;

  [[nodiscard]] EntityKind kind() const;
  template <typename T>
  [[nodiscard]] const T* as() const { return std::get_if<T>(&geometry); }
  template <typename T>
  [[nodiscard]] T* as() { return std::get_if<T>(&geometry); }

  friend bool operator==(const Entity&, const Entity&) = default;
};

// --- constraints --------------------------------------------------------

// The constraints a sketch can hold. The comment on each lists the forms its
// `refs` may take, in order; `value` is required where noted.
enum class ConstraintKind {
  Coincident,          // (point, point)
  PointOnCurve,        // (point, line | circle | arc)
  Horizontal,          // (line) | (point, point)
  Vertical,            // (line) | (point, point)
  Parallel,            // (line, line)
  Perpendicular,       // (line, line)
  Tangent,             // (line, circle | arc) | (circle | arc, circle | arc)
  Equal,               // (line, line): lengths | (circle | arc, circle | arc): radii
  Distance,            // value: (point, point) | (point, line) | (line): its length
  HorizontalDistance,  // value: (point, point): x of the second minus x of the first
  VerticalDistance,    // value: (point, point): y of the second minus y of the first
  Angle,               // value, radians: (line): from the sketch's x axis |
                       //                 (line, line): from the first to the second
  Radius,              // value: (circle | arc)
  Diameter,            // value: (circle | arc)
  Symmetric,           // (point, point, line | point): the first two mirror about the third
  Fix,                 // (point | line | circle | arc): keep it where it is now
};

std::string_view to_string(ConstraintKind kind);
std::optional<ConstraintKind> parse_constraint_kind(std::string_view text);

// Whether a constraint of this kind carries a dimension value.
bool is_dimensional(ConstraintKind kind);

// A dimension's value. `expression` is kept for M3's variables and equations,
// which will evaluate it into `value`; until then it is stored and
// round-tripped, and the solver uses `value` alone.
struct Dimension {
  double value = 0.0;
  std::string expression;

  friend bool operator==(const Dimension&, const Dimension&) = default;
};

struct Constraint {
  Id id;
  ConstraintKind kind = ConstraintKind::Coincident;
  std::vector<Id> refs;
  // Set for dimensional kinds only (see is_dimensional()).
  std::optional<Dimension> dimension;
  // Optional user-facing name; messages fall back to a generated label.
  std::string name;

  friend bool operator==(const Constraint&, const Constraint&) = default;
};

// --- the sketch ---------------------------------------------------------

class Sketch {
 public:
  // A sketch with a fresh random id.
  Sketch();
  explicit Sketch(Id id, std::string name = {}, SketchPlane plane = {});

  [[nodiscard]] const Id& id() const { return id_; }
  [[nodiscard]] const std::string& name() const { return name_; }
  void set_name(std::string name) { name_ = std::move(name); }
  [[nodiscard]] const SketchPlane& plane() const { return plane_; }
  void set_plane(const SketchPlane& plane) { plane_ = plane; }

  // --- adding geometry. Each returns the new entity's id (random unless
  // given). The overloads taking positions create the points too.
  Id add_point(dvec2 position, bool construction = false, Id id = {});
  Id add_line(Id start, Id end, bool construction = false, Id id = {});
  Id add_line(dvec2 start, dvec2 end, bool construction = false);
  Id add_circle(Id center, double radius, bool construction = false, Id id = {});
  Id add_circle(dvec2 center, double radius, bool construction = false);
  Id add_arc(Id center, Id start, Id end, bool construction = false, Id id = {});
  // An arc about `center` from angle `start_angle` counter-clockwise to
  // `end_angle` (radians).
  Id add_arc(dvec2 center, double radius, double start_angle, double end_angle,
             bool construction = false);

  // Adds an entity as given (used by deserialisation). Its id must be new.
  Id add_entity(Entity entity);

  // --- adding constraints. `value` is required for dimensional kinds and
  // ignored otherwise.
  Id add_constraint(ConstraintKind kind, std::vector<Id> refs,
                    std::optional<double> value = std::nullopt, Id id = {});
  Id add_constraint(Constraint constraint);

  // Shorthands for the common forms.
  Id coincident(Id a, Id b) { return add_constraint(ConstraintKind::Coincident, {a, b}); }
  Id point_on(Id point, Id curve) {
    return add_constraint(ConstraintKind::PointOnCurve, {point, curve});
  }
  Id horizontal(Id line) { return add_constraint(ConstraintKind::Horizontal, {line}); }
  Id vertical(Id line) { return add_constraint(ConstraintKind::Vertical, {line}); }
  Id parallel(Id a, Id b) { return add_constraint(ConstraintKind::Parallel, {a, b}); }
  Id perpendicular(Id a, Id b) { return add_constraint(ConstraintKind::Perpendicular, {a, b}); }
  Id tangent(Id a, Id b) { return add_constraint(ConstraintKind::Tangent, {a, b}); }
  Id equal(Id a, Id b) { return add_constraint(ConstraintKind::Equal, {a, b}); }
  Id distance(Id a, Id b, double d) { return add_constraint(ConstraintKind::Distance, {a, b}, d); }
  Id length(Id line, double d) { return add_constraint(ConstraintKind::Distance, {line}, d); }
  Id radius(Id curve, double r) { return add_constraint(ConstraintKind::Radius, {curve}, r); }
  Id diameter(Id curve, double d) { return add_constraint(ConstraintKind::Diameter, {curve}, d); }
  Id angle(Id a, Id b, double radians) {
    return add_constraint(ConstraintKind::Angle, {a, b}, radians);
  }
  Id symmetric(Id a, Id b, Id about) {
    return add_constraint(ConstraintKind::Symmetric, {a, b, about});
  }
  Id fix(Id entity) { return add_constraint(ConstraintKind::Fix, {entity}); }

  // --- removal. Removing an entity leaves constraints that refer to it in
  // place; validate() reports them as broken references.
  bool remove_entity(const Id& id);
  bool remove_constraint(const Id& id);

  // --- lookup
  [[nodiscard]] const std::vector<Entity>& entities() const { return entities_; }
  [[nodiscard]] const std::vector<Constraint>& constraints() const { return constraints_; }
  [[nodiscard]] const Entity* find_entity(const Id& id) const;
  [[nodiscard]] Entity* find_entity(const Id& id);
  [[nodiscard]] const Constraint* find_constraint(const Id& id) const;
  [[nodiscard]] Constraint* find_constraint(const Id& id);

  // The position of a point entity; nullopt when `id` isn't a point.
  [[nodiscard]] std::optional<dvec2> point(const Id& id) const;
  // Moves a point entity. Returns false when `id` isn't a point.
  bool set_point(const Id& id, dvec2 position);

  // Sets a circle's radius. Returns false when `id` isn't a circle.
  bool set_radius(const Id& circle, double radius);

  // The value of a dimensional constraint's dimension.
  bool set_dimension(const Id& constraint, double value);

  // A label for messages: the name if set, else "<Kind> <n>" counting
  // entities (or constraints) of that kind in order, such as "Line 3".
  [[nodiscard]] std::string label(const Id& id) const;
  [[nodiscard]] core::SubjectRef subject(const Id& id) const;
  [[nodiscard]] core::SubjectRef sketch_subject() const;

  // Structural errors ("sketch.reference.*", "sketch.entity.*",
  // "sketch.constraint.*"); empty for a well-formed sketch.
  [[nodiscard]] Diagnostics validate() const;

  friend bool operator==(const Sketch&, const Sketch&) = default;

 private:
  Id id_;
  std::string name_;
  SketchPlane plane_;
  std::vector<Entity> entities_;
  std::vector<Constraint> constraints_;
};

// --- derived geometry ---------------------------------------------------

// Resolved geometry of a curve, in sketch coordinates.
struct LineGeometry {
  dvec2 start;
  dvec2 end;
};
struct ArcGeometry {
  dvec2 center;
  double radius = 0;
  double start_angle = 0;  // radians
  double end_angle = 0;    // radians, start_angle < end_angle <= start_angle + 2π
  [[nodiscard]] dvec2 start() const;
  [[nodiscard]] dvec2 end() const;
  [[nodiscard]] dvec2 at(double angle) const;
};
struct CircleGeometry {
  dvec2 center;
  double radius = 0;
};

std::optional<LineGeometry> line_geometry(const Sketch& sketch, const Id& line);
std::optional<ArcGeometry> arc_geometry(const Sketch& sketch, const Id& arc);
std::optional<CircleGeometry> circle_geometry(const Sketch& sketch, const Id& circle);

}  // namespace vs::sketch

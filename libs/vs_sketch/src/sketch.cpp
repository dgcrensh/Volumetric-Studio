#include "vs/sketch/sketch.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <unordered_set>

#include <fmt/format.h>

#include "forms.hpp"

namespace vs::sketch {

// --- placement ----------------------------------------------------------

dvec3 SketchPlane::normal() const { return core::normalize(core::cross(x_axis, y_axis)); }

dvec3 SketchPlane::to_model(const dvec2& p) const {
  return origin + x_axis * p.x + y_axis * p.y;
}

// --- names --------------------------------------------------------------

namespace {

struct KindName {
  ConstraintKind kind;
  std::string_view key;    // for files
  std::string_view label;  // for messages
};

constexpr std::array<KindName, 16> kConstraintNames{{
    {ConstraintKind::Coincident, "coincident", "Coincident"},
    {ConstraintKind::PointOnCurve, "point_on_curve", "Point on curve"},
    {ConstraintKind::Horizontal, "horizontal", "Horizontal"},
    {ConstraintKind::Vertical, "vertical", "Vertical"},
    {ConstraintKind::Parallel, "parallel", "Parallel"},
    {ConstraintKind::Perpendicular, "perpendicular", "Perpendicular"},
    {ConstraintKind::Tangent, "tangent", "Tangent"},
    {ConstraintKind::Equal, "equal", "Equal"},
    {ConstraintKind::Distance, "distance", "Distance"},
    {ConstraintKind::HorizontalDistance, "horizontal_distance", "Horizontal distance"},
    {ConstraintKind::VerticalDistance, "vertical_distance", "Vertical distance"},
    {ConstraintKind::Angle, "angle", "Angle"},
    {ConstraintKind::Radius, "radius", "Radius"},
    {ConstraintKind::Diameter, "diameter", "Diameter"},
    {ConstraintKind::Symmetric, "symmetric", "Symmetric"},
    {ConstraintKind::Fix, "fix", "Fix"},
}};

std::string_view constraint_label(ConstraintKind kind) {
  for (const auto& n : kConstraintNames) {
    if (n.kind == kind) return n.label;
  }
  return "Constraint";
}

std::string_view entity_label(EntityKind kind) {
  switch (kind) {
    case EntityKind::Point: return "Point";
    case EntityKind::Line: return "Line";
    case EntityKind::Circle: return "Circle";
    case EntityKind::Arc: return "Arc";
    case EntityKind::Bezier: return "Curve";
  }
  return "Entity";
}

}  // namespace

std::string_view to_string(EntityKind kind) {
  switch (kind) {
    case EntityKind::Point: return "point";
    case EntityKind::Line: return "line";
    case EntityKind::Circle: return "circle";
    case EntityKind::Arc: return "arc";
    case EntityKind::Bezier: return "bezier";
  }
  return "unknown";
}

std::string_view to_string(ConstraintKind kind) {
  for (const auto& n : kConstraintNames) {
    if (n.kind == kind) return n.key;
  }
  return "unknown";
}

std::optional<ConstraintKind> parse_constraint_kind(std::string_view text) {
  for (const auto& n : kConstraintNames) {
    if (n.key == text) return n.kind;
  }
  return std::nullopt;
}

bool is_dimensional(ConstraintKind kind) {
  switch (kind) {
    case ConstraintKind::Distance:
    case ConstraintKind::HorizontalDistance:
    case ConstraintKind::VerticalDistance:
    case ConstraintKind::Angle:
    case ConstraintKind::Radius:
    case ConstraintKind::Diameter:
      return true;
    default:
      return false;
  }
}

EntityKind Entity::kind() const {
  return static_cast<EntityKind>(geometry.index());
}

// --- the sketch ---------------------------------------------------------

namespace {
Id fresh(const Id& id) { return id.is_null() ? Id::random() : id; }
}  // namespace

Sketch::Sketch() : id_(Id::random()) {}

Sketch::Sketch(Id id, std::string name, SketchPlane plane)
    : id_(id), name_(std::move(name)), plane_(plane) {}

Id Sketch::add_entity(Entity entity) {
  entity.id = fresh(entity.id);
  entities_.push_back(std::move(entity));
  return entities_.back().id;
}

Id Sketch::add_point(dvec2 position, bool construction, Id id) {
  return add_entity({id, Point{position}, construction, {}});
}

Id Sketch::add_line(Id start, Id end, bool construction, Id id) {
  return add_entity({id, Line{start, end}, construction, {}});
}

Id Sketch::add_line(dvec2 start, dvec2 end, bool construction) {
  const Id a = add_point(start, construction);
  const Id b = add_point(end, construction);
  return add_line(a, b, construction);
}

Id Sketch::add_circle(Id center, double radius, bool construction, Id id) {
  return add_entity({id, Circle{center, radius}, construction, {}});
}

Id Sketch::add_circle(dvec2 center, double radius, bool construction) {
  return add_circle(add_point(center, construction), radius, construction);
}

Id Sketch::add_arc(Id center, Id start, Id end, bool construction, Id id) {
  return add_entity({id, Arc{center, start, end}, construction, {}});
}

Id Sketch::add_arc(dvec2 center, double radius, double start_angle, double end_angle,
                   bool construction) {
  const Id c = add_point(center, construction);
  const Id s = add_point(
      center + dvec2{std::cos(start_angle), std::sin(start_angle)} * radius, construction);
  const Id e = add_point(center + dvec2{std::cos(end_angle), std::sin(end_angle)} * radius,
                         construction);
  return add_arc(c, s, e, construction);
}

Id Sketch::add_bezier(Id start, Id control1, Id control2, Id end, bool construction, Id id) {
  return add_entity({id, Bezier{start, control1, control2, end}, construction, {}});
}

Id Sketch::add_bezier(dvec2 start, dvec2 control1, dvec2 control2, dvec2 end,
                      bool construction) {
  const Id a = add_point(start, construction);
  const Id b = add_point(control1, construction);
  const Id c = add_point(control2, construction);
  const Id d = add_point(end, construction);
  return add_bezier(a, b, c, d, construction);
}

Id Sketch::add_constraint(Constraint constraint) {
  constraint.id = fresh(constraint.id);
  constraints_.push_back(std::move(constraint));
  return constraints_.back().id;
}

Id Sketch::add_constraint(ConstraintKind kind, std::vector<Id> refs, std::optional<double> value,
                          Id id) {
  Constraint c;
  c.id = id;
  c.kind = kind;
  c.refs = std::move(refs);
  if (is_dimensional(kind)) c.dimension = Dimension{value.value_or(0.0), {}};
  return add_constraint(std::move(c));
}

bool Sketch::remove_entity(const Id& id) {
  return std::erase_if(entities_, [&](const Entity& e) { return e.id == id; }) > 0;
}

bool Sketch::remove_constraint(const Id& id) {
  return std::erase_if(constraints_, [&](const Constraint& c) { return c.id == id; }) > 0;
}

const Entity* Sketch::find_entity(const Id& id) const {
  for (const auto& e : entities_) {
    if (e.id == id) return &e;
  }
  return nullptr;
}

Entity* Sketch::find_entity(const Id& id) {
  return const_cast<Entity*>(std::as_const(*this).find_entity(id));
}

const Constraint* Sketch::find_constraint(const Id& id) const {
  for (const auto& c : constraints_) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

Constraint* Sketch::find_constraint(const Id& id) {
  return const_cast<Constraint*>(std::as_const(*this).find_constraint(id));
}

std::optional<dvec2> Sketch::point(const Id& id) const {
  const Entity* e = find_entity(id);
  if (e == nullptr) return std::nullopt;
  const Point* p = e->as<Point>();
  if (p == nullptr) return std::nullopt;
  return p->position;
}

bool Sketch::set_point(const Id& id, dvec2 position) {
  Entity* e = find_entity(id);
  if (e == nullptr) return false;
  Point* p = e->as<Point>();
  if (p == nullptr) return false;
  p->position = position;
  return true;
}

bool Sketch::set_radius(const Id& id, double radius) {
  Entity* e = find_entity(id);
  if (e == nullptr) return false;
  Circle* c = e->as<Circle>();
  if (c == nullptr) return false;
  c->radius = radius;
  return true;
}

bool Sketch::set_dimension(const Id& constraint, double value) {
  Constraint* c = find_constraint(constraint);
  if (c == nullptr || !c->dimension) return false;
  c->dimension->value = value;
  return true;
}

std::string Sketch::label(const Id& id) const {
  if (const Entity* target = find_entity(id)) {
    if (!target->name.empty()) return target->name;
    std::size_t n = 0;
    for (const auto& e : entities_) {
      if (e.kind() == target->kind()) ++n;
      if (&e == target) break;
    }
    return fmt::format("{} {}", entity_label(target->kind()), n);
  }
  if (const Constraint* target = find_constraint(id)) {
    if (!target->name.empty()) return target->name;
    std::size_t n = 0;
    for (const auto& c : constraints_) {
      if (c.kind == target->kind) ++n;
      if (&c == target) break;
    }
    return fmt::format("{} {}", constraint_label(target->kind), n);
  }
  if (id == id_) return name_.empty() ? std::string("Sketch") : name_;
  return id.to_string();
}

core::SubjectRef Sketch::subject(const Id& id) const {
  core::SubjectKind kind = core::SubjectKind::SketchEntity;
  if (find_constraint(id) != nullptr) kind = core::SubjectKind::Constraint;
  if (id == id_) kind = core::SubjectKind::Sketch;
  return {kind, id, label(id)};
}

core::SubjectRef Sketch::sketch_subject() const { return subject(id_); }

// --- validation ---------------------------------------------------------

namespace {

void check_point_ref(const Sketch& sketch, const Entity& owner, const Id& ref,
                     std::string_view role, Diagnostics& out) {
  const Entity* target = sketch.find_entity(ref);
  if (target == nullptr) {
    out.add(Diagnostic::error(
        "sketch.reference.missing",
        fmt::format("{} refers to a {} that is not in the sketch", sketch.label(owner.id), role),
        {sketch.subject(owner.id), sketch.sketch_subject()}));
  } else if (target->kind() != EntityKind::Point) {
    out.add(Diagnostic::error(
        "sketch.reference.wrong_kind",
        fmt::format("{}'s {} must be a point, not {}", sketch.label(owner.id), role,
                    sketch.label(ref)),
        {sketch.subject(owner.id), sketch.subject(ref), sketch.sketch_subject()}));
  }
}

}  // namespace

Diagnostics Sketch::validate() const {
  Diagnostics out;

  std::unordered_set<Id> seen;
  seen.insert(id_);
  auto check_unique = [&](const Id& id) {
    if (id.is_null() || !seen.insert(id).second) {
      out.add(Diagnostic::error("sketch.entity.duplicate_id",
                                fmt::format("The id {} is null or used twice", id.to_string()),
                                {subject(id), sketch_subject()}));
    }
  };

  for (const auto& e : entities_) {
    check_unique(e.id);
    std::visit(
        [&](const auto& g) {
          using T = std::decay_t<decltype(g)>;
          if constexpr (std::is_same_v<T, Point>) {
            if (!std::isfinite(g.position.x) || !std::isfinite(g.position.y)) {
              out.add(Diagnostic::error("sketch.entity.not_finite",
                                        fmt::format("{} has a non-finite position", label(e.id)),
                                        {subject(e.id), sketch_subject()}));
            }
          } else if constexpr (std::is_same_v<T, Line>) {
            check_point_ref(*this, e, g.start, "start point", out);
            check_point_ref(*this, e, g.end, "end point", out);
            if (g.start == g.end) {
              out.add(Diagnostic::error("sketch.entity.degenerate",
                                        fmt::format("{} starts and ends at the same point",
                                                    label(e.id)),
                                        {subject(e.id), sketch_subject()}));
            }
          } else if constexpr (std::is_same_v<T, Circle>) {
            check_point_ref(*this, e, g.center, "centre", out);
            if (!std::isfinite(g.radius) || g.radius <= 0) {
              out.add(Diagnostic::error("sketch.entity.bad_radius",
                                        fmt::format("{} needs a positive radius", label(e.id)),
                                        {subject(e.id), sketch_subject()}));
            }
          } else if constexpr (std::is_same_v<T, Arc>) {
            check_point_ref(*this, e, g.center, "centre", out);
            check_point_ref(*this, e, g.start, "start point", out);
            check_point_ref(*this, e, g.end, "end point", out);
            if (g.start == g.end || g.center == g.start || g.center == g.end) {
              out.add(Diagnostic::error("sketch.entity.degenerate",
                                        fmt::format("{} reuses one point for two roles",
                                                    label(e.id)),
                                        {subject(e.id), sketch_subject()}));
            }
          } else if constexpr (std::is_same_v<T, Bezier>) {
            check_point_ref(*this, e, g.start, "start point", out);
            check_point_ref(*this, e, g.control1, "first handle", out);
            check_point_ref(*this, e, g.control2, "second handle", out);
            check_point_ref(*this, e, g.end, "end point", out);
            const std::array<Id, 4> ids{g.start, g.control1, g.control2, g.end};
            bool repeated = false;
            for (std::size_t i = 0; i < ids.size(); ++i) {
              for (std::size_t j = i + 1; j < ids.size(); ++j) repeated |= ids[i] == ids[j];
            }
            if (repeated) {
              out.add(Diagnostic::error("sketch.entity.degenerate",
                                        fmt::format("{} reuses one point for two roles",
                                                    label(e.id)),
                                        {subject(e.id), sketch_subject()}));
            }
          }
        },
        e.geometry);
  }

  for (const auto& c : constraints_) {
    check_unique(c.id);
    for (const auto& ref : c.refs) {
      if (find_entity(ref) == nullptr) {
        out.add(Diagnostic::error(
            "sketch.reference.missing",
            fmt::format("{} refers to geometry that is not in the sketch", label(c.id)),
            {subject(c.id), sketch_subject()}));
      }
    }
    if (out.has_errors() && std::ranges::any_of(c.refs, [&](const Id& r) {
          return find_entity(r) == nullptr;
        })) {
      continue;
    }
    if (!detail::match_form(*this, c)) {
      out.add(Diagnostic::error(
          "sketch.constraint.bad_form",
          fmt::format("{} does not apply to the geometry it names", label(c.id)),
          {subject(c.id), sketch_subject()}));
    }
    if (is_dimensional(c.kind)) {
      if (!c.dimension || !std::isfinite(c.dimension->value)) {
        out.add(Diagnostic::error("sketch.constraint.bad_value",
                                  fmt::format("{} needs a finite value", label(c.id)),
                                  {subject(c.id), sketch_subject()}));
      } else if ((c.kind == ConstraintKind::Radius || c.kind == ConstraintKind::Diameter) &&
                 c.dimension->value <= 0) {
        out.add(Diagnostic::error("sketch.constraint.bad_value",
                                  fmt::format("{} must be positive", label(c.id)),
                                  {subject(c.id), sketch_subject()}));
      } else if (c.kind == ConstraintKind::Distance && c.dimension->value < 0) {
        out.add(Diagnostic::error("sketch.constraint.bad_value",
                                  fmt::format("{} can't be negative", label(c.id)),
                                  {subject(c.id), sketch_subject()}));
      }
    }
  }
  return out;
}

// --- derived geometry ---------------------------------------------------

dvec2 ArcGeometry::at(double angle) const {
  return center + dvec2{std::cos(angle), std::sin(angle)} * radius;
}
dvec2 ArcGeometry::start() const { return at(start_angle); }
dvec2 ArcGeometry::end() const { return at(end_angle); }

std::optional<LineGeometry> line_geometry(const Sketch& sketch, const Id& line) {
  const Entity* e = sketch.find_entity(line);
  if (e == nullptr || e->as<Line>() == nullptr) return std::nullopt;
  const auto a = sketch.point(e->as<Line>()->start);
  const auto b = sketch.point(e->as<Line>()->end);
  if (!a || !b) return std::nullopt;
  return LineGeometry{*a, *b};
}

std::optional<ArcGeometry> arc_geometry(const Sketch& sketch, const Id& arc) {
  const Entity* e = sketch.find_entity(arc);
  if (e == nullptr || e->as<Arc>() == nullptr) return std::nullopt;
  const Arc& g = *e->as<Arc>();
  const auto c = sketch.point(g.center);
  const auto s = sketch.point(g.start);
  const auto t = sketch.point(g.end);
  if (!c || !s || !t) return std::nullopt;
  ArcGeometry out;
  out.center = *c;
  out.radius = core::distance(*s, *c);
  out.start_angle = std::atan2(s->y - c->y, s->x - c->x);
  out.end_angle = std::atan2(t->y - c->y, t->x - c->x);
  constexpr double kTwoPi = 2.0 * std::numbers::pi;
  while (out.end_angle <= out.start_angle) out.end_angle += kTwoPi;
  while (out.end_angle > out.start_angle + kTwoPi) out.end_angle -= kTwoPi;
  return out;
}

std::optional<CircleGeometry> circle_geometry(const Sketch& sketch, const Id& circle) {
  const Entity* e = sketch.find_entity(circle);
  if (e == nullptr || e->as<Circle>() == nullptr) return std::nullopt;
  const auto c = sketch.point(e->as<Circle>()->center);
  if (!c) return std::nullopt;
  return CircleGeometry{*c, e->as<Circle>()->radius};
}

dvec2 BezierGeometry::at(double t) const {
  const double u = 1.0 - t;
  const dvec2 a = p[0] * u + p[1] * t;
  const dvec2 b = p[1] * u + p[2] * t;
  const dvec2 c = p[2] * u + p[3] * t;
  const dvec2 d = a * u + b * t;
  const dvec2 e = b * u + c * t;
  return d * u + e * t;
}

dvec2 BezierGeometry::derivative(double t) const {
  const double u = 1.0 - t;
  const dvec2 a = (p[1] - p[0]) * 3.0;
  const dvec2 b = (p[2] - p[1]) * 3.0;
  const dvec2 c = (p[3] - p[2]) * 3.0;
  return (a * u + b * t) * u + (b * u + c * t) * t;
}

dvec2 BezierGeometry::tangent(bool at_end) const {
  // From the end along the nearest control point that differs from it.
  const std::array<dvec2, 3> toward =
      at_end ? std::array<dvec2, 3>{p[3] - p[2], p[3] - p[1], p[3] - p[0]}
             : std::array<dvec2, 3>{p[1] - p[0], p[2] - p[0], p[3] - p[0]};
  for (const auto& d : toward) {
    const double len = core::length(d);
    if (len > 0.0) return d / len;
  }
  return {};
}

double BezierGeometry::area_term() const {
  const auto cr = [&](int i, int j) { return core::cross(p[i], p[j]); };
  return (6.0 * cr(0, 1) + 3.0 * cr(0, 2) + cr(0, 3) + 3.0 * cr(1, 2) + 3.0 * cr(1, 3) +
          6.0 * cr(2, 3)) /
         20.0;
}

int BezierGeometry::steps_within(double deflection, int min_steps) const {
  // A chord over a step h strays at most h² max|B''| / 8 from the curve,
  // and |B''| <= 6 max(|p0 - 2 p1 + p2|, |p1 - 2 p2 + p3|).
  const double m = 6.0 * std::max(core::length(p[0] - p[1] * 2.0 + p[2]),
                                  core::length(p[1] - p[2] * 2.0 + p[3]));
  constexpr int kMaxSteps = 65536;
  int n = min_steps;
  if (deflection > 0.0 && m > 0.0) {
    const double need = std::ceil(std::sqrt(m / (8.0 * deflection)));
    if (need > n) n = need >= kMaxSteps ? kMaxSteps : static_cast<int>(need);
  }
  return std::min(n, kMaxSteps);
}

std::optional<BezierGeometry> bezier_geometry(const Sketch& sketch, const Id& bezier) {
  const Entity* e = sketch.find_entity(bezier);
  if (e == nullptr || e->as<Bezier>() == nullptr) return std::nullopt;
  const Bezier& g = *e->as<Bezier>();
  BezierGeometry out;
  const std::array<Id, 4> ids{g.start, g.control1, g.control2, g.end};
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto pt = sketch.point(ids[i]);
    if (!pt) return std::nullopt;
    out.p[i] = *pt;
  }
  return out;
}

}  // namespace vs::sketch

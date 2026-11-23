#pragma once

// The reference forms each constraint kind accepts, shared by validation and
// the solver so the two can't disagree.

#include <array>
#include <optional>
#include <vector>

#include "vs/sketch/sketch.hpp"

namespace vs::sketch::detail {

// The entity kinds of a constraint's references, in order. Empty when a
// reference is missing.
inline std::vector<EntityKind> ref_kinds(const Sketch& sketch, const Constraint& c) {
  std::vector<EntityKind> kinds;
  kinds.reserve(c.refs.size());
  for (const auto& r : c.refs) {
    const Entity* e = sketch.find_entity(r);
    if (e == nullptr) return {};
    kinds.push_back(e->kind());
  }
  return kinds;
}

inline bool is_round(EntityKind k) { return k == EntityKind::Circle || k == EntityKind::Arc; }
inline bool is_curve(EntityKind k) { return k != EntityKind::Point; }

// The end points of a line, arc or Bézier (null ids for anything else).
inline std::array<Id, 2> end_points(const Sketch& sketch, const Id& curve) {
  const Entity* e = sketch.find_entity(curve);
  if (e == nullptr) return {};
  if (const auto* l = e->as<Line>()) return {l->start, l->end};
  if (const auto* a = e->as<Arc>()) return {a->start, a->end};
  if (const auto* b = e->as<Bezier>()) return {b->start, b->end};
  return {};
}

// A point that is an end (not a centre or handle) of both curves `a` and `b`.
inline std::optional<Id> shared_end(const Sketch& sketch, const Id& a, const Id& b) {
  for (const auto& p : end_points(sketch, a)) {
    if (p.is_null()) continue;
    for (const auto& q : end_points(sketch, b)) {
      if (p == q) return p;
    }
  }
  return std::nullopt;
}

// The handle of Bézier `bezier` at its end point `end`: the control point
// next to it.
inline Id handle_at(const Bezier& bezier, const Id& end) {
  return end == bezier.start ? bezier.control1 : bezier.control2;
}

// True when `c`'s references fit one of its kind's forms (see sketch.hpp).
inline bool match_form(const Sketch& sketch, const Constraint& c) {
  using K = EntityKind;
  const auto k = ref_kinds(sketch, c);
  if (k.size() != c.refs.size() || k.empty()) return false;
  const auto n = k.size();
  // References must be distinct.
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      if (c.refs[i] == c.refs[j]) return false;
    }
  }
  switch (c.kind) {
    case ConstraintKind::Coincident:
    case ConstraintKind::HorizontalDistance:
    case ConstraintKind::VerticalDistance:
      return n == 2 && k[0] == K::Point && k[1] == K::Point;
    case ConstraintKind::PointOnCurve:
      return n == 2 && k[0] == K::Point && is_curve(k[1]) && k[1] != K::Bezier;
    case ConstraintKind::Horizontal:
    case ConstraintKind::Vertical:
      return (n == 1 && k[0] == K::Line) || (n == 2 && k[0] == K::Point && k[1] == K::Point);
    case ConstraintKind::Parallel:
    case ConstraintKind::Perpendicular:
      return n == 2 && k[0] == K::Line && k[1] == K::Line;
    case ConstraintKind::Tangent:
      if (n != 2) return false;
      if (k[0] == K::Bezier || k[1] == K::Bezier) {
        // Only where the two curves share an end point (decision 0040).
        const auto partner_ok = [](K kind) {
          return kind == K::Line || kind == K::Arc || kind == K::Bezier;
        };
        return partner_ok(k[0]) && partner_ok(k[1]) &&
               shared_end(sketch, c.refs[0], c.refs[1]).has_value();
      }
      return (k[0] == K::Line && is_round(k[1])) || (is_round(k[0]) && is_round(k[1]));
    case ConstraintKind::Equal:
      return n == 2 && ((k[0] == K::Line && k[1] == K::Line) ||
                        (is_round(k[0]) && is_round(k[1])));
    case ConstraintKind::Distance:
      return (n == 1 && k[0] == K::Line) ||
             (n == 2 && k[0] == K::Point && (k[1] == K::Point || k[1] == K::Line));
    case ConstraintKind::Angle:
      return (n == 1 && k[0] == K::Line) || (n == 2 && k[0] == K::Line && k[1] == K::Line);
    case ConstraintKind::Radius:
    case ConstraintKind::Diameter:
      return n == 1 && is_round(k[0]);
    case ConstraintKind::Symmetric:
      return n == 3 && k[0] == K::Point && k[1] == K::Point &&
             (k[2] == K::Line || k[2] == K::Point);
    case ConstraintKind::Fix:
      return n == 1;
  }
  return false;
}

}  // namespace vs::sketch::detail

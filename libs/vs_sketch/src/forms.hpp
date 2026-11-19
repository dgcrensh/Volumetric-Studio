#pragma once

// The reference forms each constraint kind accepts, shared by validation and
// the solver so the two can't disagree.

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
      return n == 2 && k[0] == K::Point && is_curve(k[1]);
    case ConstraintKind::Horizontal:
    case ConstraintKind::Vertical:
      return (n == 1 && k[0] == K::Line) || (n == 2 && k[0] == K::Point && k[1] == K::Point);
    case ConstraintKind::Parallel:
    case ConstraintKind::Perpendicular:
      return n == 2 && k[0] == K::Line && k[1] == K::Line;
    case ConstraintKind::Tangent:
      return n == 2 && ((k[0] == K::Line && is_round(k[1])) ||
                        (is_round(k[0]) && is_round(k[1])));
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

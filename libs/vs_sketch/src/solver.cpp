// The PlaneGCS implementation of SketchSolver.
//
// Each solve builds a fresh GCS::System from the sketch:
//   - every point contributes two unknowns (x, y); circles add their radius;
//     Béziers add nothing, being their four points (S1 phase 4: a Bézier a
//     constraint reaches along its length is also a one-span cubic
//     GCS::BSpline over the same four points, with constant knots and
//     weights);
//     arcs add their radius and start and end angles, tied to their centre,
//     start and end points by PlaneGCS's arc rules;
//   - every sketch constraint becomes one or more GCS constraints carrying
//     the tag (index + 1), so PlaneGCS's diagnosis can be mapped back to it;
//   - dimension values and fixed positions are constants: pointers to doubles
//     that are not declared unknown.
//   - a point on a Bézier, and a tangency anywhere along one, add a curve
//     parameter (and for a tangency a contact point) as hidden unknowns,
//     started at the nearest place on the curve; a solve that puts one off
//     the curve's ends fails (sketch.solve.beyond_curve).
// Dragging adds two temporary constraints (tag -1) pulling the point to the
// target. PlaneGCS solves those with lower priority than the sketch's own,
// so the sketch's constraints keep holding and the point gets as close as
// they allow.

#include "vs/sketch/solver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <numbers>
#include <optional>

#include <set>
#include <unordered_map>

#include <fmt/format.h>

#include "GCS.h"
#include "forms.hpp"

namespace vs::sketch {

std::string_view to_string(SolveStatus status) {
  switch (status) {
    case SolveStatus::Solved: return "solved";
    case SolveStatus::UnderConstrained: return "under-constrained";
    case SolveStatus::OverConstrained: return "over-constrained";
    case SolveStatus::Failed: return "failed";
  }
  return "unknown";
}

namespace {

// How far a constraint left out as redundant may miss once the others are
// solved (in its own scaled units, as PlaneGCS reports it).
constexpr double kRedundantCheck = 1e-7;


// The GCS system for one sketch, and the storage its pointers refer to.
class GcsModel {
 public:
  explicit GcsModel(Sketch& sketch) : sketch_(sketch) {}

  void build();
  void add_drag(const Id& point, dvec2 target);
  SolveReport run();

 private:
  // Unknowns. std::deque keeps element addresses stable as it grows.
  double* unknown(double value, const Id& owner) {
    unknowns_.push_back(value);
    double* p = &unknowns_.back();
    plist_.push_back(p);
    owner_[p] = owner;
    return p;
  }
  double* constant(double value) {
    constants_.push_back(value);
    return &constants_.back();
  }

  GCS::Point& point(const Id& id) { return points_.at(id); }
  GCS::Line& line(const Id& id) { return lines_.at(id); }
  GCS::Circle& circle(const Id& id) { return circles_.at(id); }
  GCS::Arc& arc(const Id& id) { return arcs_.at(id); }
  GCS::BSpline& bspline(const Id& id);
  // A curve parameter of Bézier `b` (hidden, owned by `owner`), started at `t`.
  double* curve_param(double t, const Id& owner) {
    double* u = unknown(t, owner);
    curve_params_.push_back(u);
    return u;
  }
  void tangent_along(const Id& bezier_id, const Id& other, EntityKind other_kind, int tag);

  std::optional<Id> shared_end(const Id& a, const Id& b) const {
    return detail::shared_end(sketch_, a, b);
  }
  const Bezier& bezier(const Id& id) const { return *sketch_.find_entity(id)->as<Bezier>(); }

  void add_constraint(const Constraint& c, int tag);
  void fix_point(const Id& id, int tag);
  void write_back();
  std::vector<Id> tags_to_ids(const GCS::VEC_I& tags) const;

  Sketch& sketch_;
  GCS::System sys_;
  std::deque<double> unknowns_;
  std::deque<double> constants_;
  GCS::VEC_pD plist_;
  std::unordered_map<double*, Id> owner_;
  std::map<Id, GCS::Point> points_;
  std::map<Id, GCS::Line> lines_;
  std::map<Id, GCS::Circle> circles_;
  std::map<Id, GCS::Arc> arcs_;
  std::map<Id, GCS::BSpline> bsplines_;
  std::deque<GCS::Point> contacts_;
  std::vector<double*> curve_params_;
};

// The parameter in [0, 1] of the point of `g` nearest to where `score` is
// least: a dense sample, then a refinement by thirds.
template <typename Score>
double best_param(const BezierGeometry& g, Score&& score) {
  constexpr int kSamples = 256;
  int best = 0;
  double best_score = score(g.at(0));
  for (int i = 1; i <= kSamples; ++i) {
    const double v = score(g.at(static_cast<double>(i) / kSamples));
    if (v < best_score) {
      best_score = v;
      best = i;
    }
  }
  double lo = std::max(0.0, (best - 1.0) / kSamples), hi = std::min(1.0, (best + 1.0) / kSamples);
  for (int i = 0; i < 60; ++i) {
    const double a = lo + (hi - lo) / 3, b = hi - (hi - lo) / 3;
    if (score(g.at(a)) < score(g.at(b))) {
      hi = b;
    } else {
      lo = a;
    }
  }
  return (lo + hi) / 2;
}

GCS::BSpline& GcsModel::bspline(const Id& id) {
  if (auto it = bsplines_.find(id); it != bsplines_.end()) return it->second;
  const Bezier& z = bezier(id);
  GCS::BSpline& b = bsplines_[id];
  for (const Id& p : {z.start, z.control1, z.control2, z.end}) {
    b.poles.push_back(point(p));
    b.weights.push_back(constant(1.0));
  }
  b.knots = {constant(0.0), constant(1.0)};
  b.mult = {4, 4};
  b.degree = 3;
  b.periodic = false;
  b.start = point(z.start);
  b.end = point(z.end);
  b.setupFlattenedKnots();
  return b;
}

// Tangency anywhere along a Bézier: a contact point on both curves, where
// their normals are parallel. Three hidden unknowns (the contact and the
// Bézier's parameter there) and four equations: one degree of freedom
// taken, like any tangency.
void GcsModel::tangent_along(const Id& bezier_id, const Id& other, EntityKind other_kind, int tag) {
  const BezierGeometry g = *bezier_geometry(sketch_, bezier_id);
  double t = 0.5;
  if (other_kind == EntityKind::Line) {
    const auto l = *line_geometry(sketch_, other);
    const dvec2 d = core::normalize(l.end - l.start);
    t = best_param(g, [&](dvec2 p) { return std::abs(core::cross(d, p - l.start)); });
  } else {
    dvec2 c;
    double r = 0;
    if (other_kind == EntityKind::Circle) {
      const auto cg = *circle_geometry(sketch_, other);
      c = cg.center;
      r = cg.radius;
    } else {
      const auto ag = *arc_geometry(sketch_, other);
      c = ag.center;
      r = ag.radius;
    }
    t = best_param(g, [&](dvec2 p) { return std::abs(core::distance(p, c) - r); });
  }
  const dvec2 at = g.at(t);
  double* u = curve_param(t, bezier_id);
  contacts_.emplace_back(unknown(at.x, bezier_id), unknown(at.y, bezier_id));
  GCS::Point& contact = contacts_.back();
  GCS::BSpline& b = bspline(bezier_id);
  sys_.addConstraintPointOnBSpline(contact, b, u, tag);
  GCS::Curve* partner = nullptr;
  switch (other_kind) {
    case EntityKind::Line:
      sys_.addConstraintPointOnLine(contact, line(other), tag);
      partner = &line(other);
      break;
    case EntityKind::Circle:
      sys_.addConstraintPointOnCircle(contact, circle(other), tag);
      partner = &circle(other);
      break;
    default:
      sys_.addConstraintPointOnArc(contact, arc(other), tag);
      partner = &arc(other);
      break;
  }
  // Normals parallel, facing the way they face now (0 or a half turn).
  const GCS::DeriVector2 n1 = b.CalculateNormal(u);
  const GCS::DeriVector2 n2 = partner->CalculateNormal(contact);
  const double facing = n1.x * n2.x + n1.y * n2.y;
  sys_.addConstraintAngleViaPointAndParam(b, *partner, contact, u,
                                          constant(facing >= 0 ? 0.0 : std::numbers::pi), tag);
}

void GcsModel::build() {
  // Points first, so curves can refer to them.
  for (const auto& e : sketch_.entities()) {
    if (const auto* p = e.as<Point>()) {
      points_[e.id] = GCS::Point(unknown(p->position.x, e.id), unknown(p->position.y, e.id));
    }
  }
  for (const auto& e : sketch_.entities()) {
    if (const auto* l = e.as<Line>()) {
      GCS::Line g;
      g.p1 = point(l->start);
      g.p2 = point(l->end);
      lines_[e.id] = g;
    } else if (const auto* c = e.as<Circle>()) {
      GCS::Circle g;
      g.center = point(c->center);
      g.rad = unknown(c->radius, e.id);
      circles_[e.id] = g;
    } else if (e.as<Arc>() != nullptr) {
      const Arc& a = *e.as<Arc>();
      const auto geo = *arc_geometry(sketch_, e.id);
      GCS::Arc g;
      g.center = point(a.center);
      g.start = point(a.start);
      g.end = point(a.end);
      g.rad = unknown(geo.radius, e.id);
      g.startAngle = unknown(geo.start_angle, e.id);
      g.endAngle = unknown(geo.end_angle, e.id);
      arcs_[e.id] = g;
      // Keeps the start and end points on the arc (tag 0: not a user
      // constraint, so never reported).
      sys_.addConstraintArcRules(arcs_[e.id], 0);
    }
  }

  int tag = 0;
  for (const auto& c : sketch_.constraints()) {
    ++tag;
    add_constraint(c, tag);
  }
}

void GcsModel::fix_point(const Id& id, int tag) {
  GCS::Point& p = point(id);
  sys_.addConstraintCoordinateX(p, constant(*p.x), tag);
  sys_.addConstraintCoordinateY(p, constant(*p.y), tag);
}

void GcsModel::add_constraint(const Constraint& c, int tag) {
  using K = EntityKind;
  const auto kinds = detail::ref_kinds(sketch_, c);
  const auto& r = c.refs;
  double* value = c.dimension ? constant(c.dimension->value) : nullptr;

  switch (c.kind) {
    case ConstraintKind::Coincident:
      sys_.addConstraintP2PCoincident(point(r[0]), point(r[1]), tag);
      break;
    case ConstraintKind::PointOnCurve:
      if (kinds[1] == K::Line) {
        sys_.addConstraintPointOnLine(point(r[0]), line(r[1]), tag);
      } else if (kinds[1] == K::Bezier) {
        const BezierGeometry g = *bezier_geometry(sketch_, r[1]);
        const dvec2 p = *sketch_.point(r[0]);
        const double t = best_param(g, [&](dvec2 q) { return core::distance(p, q); });
        sys_.addConstraintPointOnBSpline(point(r[0]), bspline(r[1]), curve_param(t, r[0]), tag);
      } else if (kinds[1] == K::Circle) {
        sys_.addConstraintPointOnCircle(point(r[0]), circle(r[1]), tag);
      } else {
        sys_.addConstraintPointOnArc(point(r[0]), arc(r[1]), tag);
      }
      break;
    case ConstraintKind::Horizontal:
      if (kinds.size() == 1) {
        sys_.addConstraintHorizontal(line(r[0]), tag);
      } else {
        sys_.addConstraintHorizontal(point(r[0]), point(r[1]), tag);
      }
      break;
    case ConstraintKind::Vertical:
      if (kinds.size() == 1) {
        sys_.addConstraintVertical(line(r[0]), tag);
      } else {
        sys_.addConstraintVertical(point(r[0]), point(r[1]), tag);
      }
      break;
    case ConstraintKind::Parallel:
      sys_.addConstraintParallel(line(r[0]), line(r[1]), tag);
      break;
    case ConstraintKind::Perpendicular:
      sys_.addConstraintPerpendicular(line(r[0]), line(r[1]), tag);
      break;
    case ConstraintKind::Tangent:
      // Curves that share an end point touch there, and tangency is the
      // well-conditioned condition at that point: the line is perpendicular
      // to the arc's radius, or the two centres and the point are collinear.
      // (PlaneGCS's general tangency, a centre-to-line distance equal to the
      // radius, pins the touching point only to about the square root of the
      // solver's tolerance.)
      if ((kinds[0] == K::Bezier || kinds[1] == K::Bezier) && !shared_end(r[0], r[1])) {
        // Anywhere along the Bézier (match_form: a line, circle or arc).
        const bool bezier_first = kinds[0] == K::Bezier;
        tangent_along(r[bezier_first ? 0 : 1], r[bezier_first ? 1 : 0], kinds[bezier_first ? 1 : 0],
                      tag);
      } else if (kinds[0] == K::Bezier || kinds[1] == K::Bezier) {
        // At their shared end. A Bézier's direction there is
        // along its handle: the handle stays on the line's line, square to
        // the arc's radius, or in line with the other Bézier's handle.
        const Id s = *shared_end(r[0], r[1]);
        const bool bezier_first = kinds[0] == K::Bezier;
        const Id& b = r[bezier_first ? 0 : 1];
        const Id& other = r[bezier_first ? 1 : 0];
        const K other_kind = kinds[bezier_first ? 1 : 0];
        GCS::Point& handle = point(detail::handle_at(bezier(b), s));
        if (other_kind == K::Line) {
          GCS::Line& l = line(other);
          sys_.addConstraintPointOnLine(handle, l.p1, l.p2, tag);
        } else if (other_kind == K::Arc) {
          sys_.addConstraintPerpendicular(handle, point(s), arc(other).center, point(s), tag);
        } else {
          GCS::Point& other_handle = point(detail::handle_at(bezier(other), s));
          sys_.addConstraintPointOnLine(point(s), handle, other_handle, tag);
        }
      } else if (const auto s = shared_end(r[0], r[1])) {
        if (kinds[0] == K::Line || kinds[1] == K::Line) {
          const bool line_first = kinds[0] == K::Line;
          GCS::Line& l = line(r[line_first ? 0 : 1]);
          GCS::Arc& a = arc(r[line_first ? 1 : 0]);
          sys_.addConstraintPerpendicular(l.p1, l.p2, a.center, point(*s), tag);
        } else {
          sys_.addConstraintPointOnLine(point(*s), arc(r[0]).center, arc(r[1]).center, tag);
        }
      } else if (kinds[0] == K::Line) {
        if (kinds[1] == K::Circle) {
          sys_.addConstraintTangent(line(r[0]), circle(r[1]), tag);
        } else {
          sys_.addConstraintTangent(line(r[0]), arc(r[1]), tag);
        }
      } else if (kinds[0] == K::Circle && kinds[1] == K::Circle) {
        sys_.addConstraintTangent(circle(r[0]), circle(r[1]), tag);
      } else if (kinds[0] == K::Arc && kinds[1] == K::Arc) {
        sys_.addConstraintTangent(arc(r[0]), arc(r[1]), tag);
      } else if (kinds[0] == K::Circle) {
        sys_.addConstraintTangent(circle(r[0]), arc(r[1]), tag);
      } else {
        sys_.addConstraintTangent(circle(r[1]), arc(r[0]), tag);
      }
      break;
    case ConstraintKind::Equal:
      if (kinds[0] == K::Line) {
        sys_.addConstraintEqualLength(line(r[0]), line(r[1]), tag);
      } else if (kinds[0] == K::Circle && kinds[1] == K::Circle) {
        sys_.addConstraintEqualRadius(circle(r[0]), circle(r[1]), tag);
      } else if (kinds[0] == K::Arc && kinds[1] == K::Arc) {
        sys_.addConstraintEqualRadius(arc(r[0]), arc(r[1]), tag);
      } else if (kinds[0] == K::Circle) {
        sys_.addConstraintEqualRadius(circle(r[0]), arc(r[1]), tag);
      } else {
        sys_.addConstraintEqualRadius(circle(r[1]), arc(r[0]), tag);
      }
      break;
    case ConstraintKind::Distance:
      if (kinds.size() == 1) {
        GCS::Line& l = line(r[0]);
        sys_.addConstraintP2PDistance(l.p1, l.p2, value, tag);
      } else if (kinds[1] == K::Point) {
        sys_.addConstraintP2PDistance(point(r[0]), point(r[1]), value, tag);
      } else {
        sys_.addConstraintP2LDistance(point(r[0]), line(r[1]), value, tag);
      }
      break;
    case ConstraintKind::HorizontalDistance:
      sys_.addConstraintDifference(point(r[0]).x, point(r[1]).x, value, tag);
      break;
    case ConstraintKind::VerticalDistance:
      sys_.addConstraintDifference(point(r[0]).y, point(r[1]).y, value, tag);
      break;
    case ConstraintKind::Angle:
      if (kinds.size() == 1) {
        GCS::Line& l = line(r[0]);
        // The overload with an angle increment: the one without drops the
        // tag (third_party/planegcs GCS.cpp), so conflicts and redundancy
        // couldn't name this constraint (plan H1).
        sys_.addConstraintP2PAngle(l.p1, l.p2, value, 0.0, tag);
      } else {
        sys_.addConstraintL2LAngle(line(r[0]), line(r[1]), value, tag);
      }
      break;
    case ConstraintKind::Radius:
      if (kinds[0] == K::Circle) {
        sys_.addConstraintCircleRadius(circle(r[0]), value, tag);
      } else {
        sys_.addConstraintArcRadius(arc(r[0]), value, tag);
      }
      break;
    case ConstraintKind::Diameter:
      if (kinds[0] == K::Circle) {
        sys_.addConstraintCircleDiameter(circle(r[0]), value, tag);
      } else {
        sys_.addConstraintArcDiameter(arc(r[0]), value, tag);
      }
      break;
    case ConstraintKind::Symmetric:
      if (kinds[2] == K::Line) {
        sys_.addConstraintP2PSymmetric(point(r[0]), point(r[1]), line(r[2]), tag);
      } else {
        sys_.addConstraintP2PSymmetric(point(r[0]), point(r[1]), point(r[2]), tag);
      }
      break;
    case ConstraintKind::Fix:
      switch (kinds[0]) {
        case K::Point:
          fix_point(r[0], tag);
          break;
        case K::Line: {
          const Line& l = *sketch_.find_entity(r[0])->as<Line>();
          fix_point(l.start, tag);
          fix_point(l.end, tag);
          break;
        }
        case K::Circle: {
          GCS::Circle& g = circle(r[0]);
          fix_point(sketch_.find_entity(r[0])->as<Circle>()->center, tag);
          sys_.addConstraintEqual(g.rad, constant(*g.rad), tag);
          break;
        }
        case K::Arc: {
          // Centre, radius and both angles: exactly the arc's freedoms.
          GCS::Arc& g = arc(r[0]);
          fix_point(sketch_.find_entity(r[0])->as<Arc>()->center, tag);
          sys_.addConstraintEqual(g.rad, constant(*g.rad), tag);
          sys_.addConstraintEqual(g.startAngle, constant(*g.startAngle), tag);
          sys_.addConstraintEqual(g.endAngle, constant(*g.endAngle), tag);
          break;
        }
        case K::Bezier: {
          const Bezier& g = bezier(r[0]);
          for (const Id& p : {g.start, g.control1, g.control2, g.end}) fix_point(p, tag);
          break;
        }
      }
      break;
  }
}

void GcsModel::add_drag(const Id& id, dvec2 target) {
  GCS::Point& p = point(id);
  sys_.addConstraintCoordinateX(p, constant(target.x), GCS::DefaultTemporaryConstraint);
  sys_.addConstraintCoordinateY(p, constant(target.y), GCS::DefaultTemporaryConstraint);
}

std::vector<Id> GcsModel::tags_to_ids(const GCS::VEC_I& tags) const {
  std::set<int> wanted(tags.begin(), tags.end());
  std::vector<Id> out;
  int tag = 0;
  for (const auto& c : sketch_.constraints()) {
    if (wanted.contains(++tag)) out.push_back(c.id);
  }
  return out;
}

void GcsModel::write_back() {
  for (const auto& [id, p] : points_) sketch_.set_point(id, {*p.x, *p.y});
  for (const auto& [id, c] : circles_) sketch_.set_radius(id, *c.rad);
}

std::string join_labels(const Sketch& sketch, const std::vector<Id>& ids) {
  std::string out;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i > 0) out += i + 1 == ids.size() ? " and " : ", ";
    out += sketch.label(ids[i]);
  }
  return out;
}

SolveReport GcsModel::run() {
  SolveReport report;
  const auto sketch_ref = sketch_.sketch_subject();

  sys_.declareUnknowns(plist_);
  sys_.initSolution(GCS::DogLeg);  // also diagnoses

  report.dof = std::max(0, sys_.dofsNumber());
  GCS::VEC_I conflicting, redundant, partially;
  sys_.getConflicting(conflicting);
  sys_.getRedundant(redundant);
  sys_.getPartiallyRedundant(partially);
  redundant.insert(redundant.end(), partially.begin(), partially.end());
  report.conflicting = tags_to_ids(conflicting);
  report.redundant = tags_to_ids(redundant);

  // Free entities: the owners of parameters the constraints don't determine.
  // With no constraints at all PlaneGCS skips the analysis; everything is free.
  std::set<Id> free;
  if (sys_.isEmptyDiagnoseMatrix()) {
    for (auto* p : plist_) free.insert(owner_.at(p));
  } else {
    GCS::VEC_pD dependent;
    sys_.getDependentParams(dependent);
    for (auto* p : dependent) {
      if (auto it = owner_.find(p); it != owner_.end()) free.insert(it->second);
    }
  }
  if (report.dof == 0) free.clear();
  for (const auto& e : sketch_.entities()) {
    if (free.contains(e.id)) report.free_entities.push_back(e.id);
  }

  if (!report.conflicting.empty()) {
    report.status = SolveStatus::OverConstrained;
    std::vector<core::SubjectRef> subjects;
    for (const auto& id : report.conflicting) subjects.push_back(sketch_.subject(id));
    subjects.push_back(sketch_ref);
    report.diagnostics.add(Diagnostic::error(
        "sketch.solve.conflicting",
        fmt::format("These constraints conflict; remove or change one of them: {}",
                    join_labels(sketch_, report.conflicting)),
        std::move(subjects)));
    return report;
  }

  // PlaneGCS solves without the redundant constraints, then checks them
  // against the parameters before the solution is applied, so a sketch whose
  // redundant constraints didn't already hold comes back "Converged". Apply
  // the solution and check every constraint instead.
  const auto all_hold = [&] {
    sys_.applySolution();
    for (int tag = 1; tag <= static_cast<int>(sketch_.constraints().size()); ++tag) {
      const double err = sys_.calculateConstraintErrorByTag(tag);
      if (!std::isnan(err) && std::abs(err) > kRedundantCheck) return false;
    }
    return true;
  };

  bool solved = false;
  for (const auto alg : {GCS::DogLeg, GCS::LevenbergMarquardt, GCS::BFGS}) {
    const int result = sys_.solve(true, alg);
    solved = result == GCS::Success ||
             (result == GCS::Converged && !report.redundant.empty() && all_hold());
    if (solved) break;
    sys_.undoSolution();
  }

  if (!solved) {
    sys_.undoSolution();
    report.status = SolveStatus::Failed;
    report.diagnostics.add(Diagnostic::error(
        "sketch.solve.failed",
        "The sketch could not be solved; its constraints may ask for impossible geometry",
        {sketch_ref}));
    return report;
  }

  // A curve parameter past the curve's ends puts a point on the curve's
  // extension, not on the curve.
  constexpr double kParamSlack = 1e-9;
  sys_.applySolution();
  for (const double* u : curve_params_) {
    if (*u < -kParamSlack || *u > 1 + kParamSlack) {
      sys_.undoSolution();
      report.status = SolveStatus::Failed;
      report.diagnostics.add(Diagnostic::error(
          "sketch.solve.beyond_curve",
          "A point on a curve, or a tangency with one, would lie beyond the curve's ends",
          {sketch_ref}));
      return report;
    }
  }

  write_back();
  report.applied = true;

  for (const auto& id : report.redundant) {
    report.diagnostics.add(Diagnostic::warning(
        "sketch.solve.redundant",
        fmt::format("{} repeats what other constraints already fix; remove it",
                    sketch_.label(id)),
        {sketch_.subject(id), sketch_ref}));
  }
  if (!report.redundant.empty()) {
    report.status = SolveStatus::OverConstrained;
  } else if (report.dof > 0) {
    report.status = SolveStatus::UnderConstrained;
  } else {
    report.status = SolveStatus::Solved;
  }
  if (report.dof > 0) {
    std::vector<core::SubjectRef> subjects;
    for (const auto& id : report.free_entities) subjects.push_back(sketch_.subject(id));
    subjects.push_back(sketch_ref);
    report.diagnostics.add(Diagnostic::info(
        "sketch.solve.under_constrained",
        fmt::format("{} degree{} of freedom remain{}", report.dof, report.dof == 1 ? "" : "s",
                    report.free_entities.empty()
                        ? std::string{}
                        : fmt::format("; free: {}", join_labels(sketch_, report.free_entities))),
        std::move(subjects)));
  }
  return report;
}

SolveReport invalid_report(Diagnostics problems) {
  SolveReport report;
  report.status = SolveStatus::Failed;
  report.diagnostics = std::move(problems);
  return report;
}

class PlaneGcsSolver final : public SketchSolver {
 public:
  SolveReport solve(Sketch& sketch, const ProductSettings& settings) override {
    return run(sketch, nullptr, {}, settings);
  }

  SolveReport drag(Sketch& sketch, const Id& point, dvec2 target,
                   const ProductSettings& settings) override {
    if (!sketch.point(point)) {
      Diagnostics d;
      d.add(Diagnostic::error("sketch.solve.drag_not_point", "Only points can be dragged",
                              {sketch.subject(point), sketch.sketch_subject()}));
      return invalid_report(std::move(d));
    }
    return run(sketch, &point, target, settings);
  }

 private:
  static SolveReport run(Sketch& sketch, const Id* drag_point, dvec2 target,
                         const ProductSettings& settings) {
    Diagnostics problems = sketch.validate();
    if (problems.has_errors()) return invalid_report(std::move(problems));

    GcsModel model(sketch);
    model.build();
    if (drag_point != nullptr) model.add_drag(*drag_point, target);
    SolveReport report = model.run();

    // A solve that leaves the working space is no solve at all.
    if (report.applied) {
      for (const auto& e : sketch.entities()) {
        if (const auto* p = e.as<Point>()) {
          if (!settings.in_working_space(sketch.plane().to_model(p->position))) {
            report.diagnostics.add(Diagnostic::warning(
                "sketch.solve.outside_working_space",
                fmt::format("{} lies outside the working space", sketch.label(e.id)),
                {sketch.subject(e.id), sketch.sketch_subject()}));
          }
        }
      }
    }
    return report;
  }
};

}  // namespace

std::unique_ptr<SketchSolver> make_solver() { return std::make_unique<PlaneGcsSolver>(); }

SolveReport solve(Sketch& sketch, const ProductSettings& settings) {
  return make_solver()->solve(sketch, settings);
}

SolveReport drag(Sketch& sketch, const Id& point, dvec2 target, const ProductSettings& settings) {
  return make_solver()->drag(sketch, point, target, settings);
}

}  // namespace vs::sketch

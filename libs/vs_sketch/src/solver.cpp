// The PlaneGCS implementation of SketchSolver.
//
// Each solve builds a fresh GCS::System from the sketch:
//   - every point contributes two unknowns (x, y); circles add their radius;
//     arcs add their radius and start and end angles, tied to their centre,
//     start and end points by PlaneGCS's arc rules;
//   - every sketch constraint becomes one or more GCS constraints carrying
//     the tag (index + 1), so PlaneGCS's diagnosis can be mapped back to it;
//   - dimension values and fixed positions are constants: pointers to doubles
//     that are not declared unknown.
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

  // A point that is an end (not the centre) of both line or arc `a` and `b`.
  std::optional<Id> shared_end(const Id& a, const Id& b) const {
    const auto ends = [&](const Id& id) -> std::array<Id, 2> {
      const Entity* e = sketch_.find_entity(id);
      if (const auto* l = e->as<Line>()) return {l->start, l->end};
      if (const auto* arc = e->as<Arc>()) return {arc->start, arc->end};
      return {};
    };
    for (const auto& p : ends(a)) {
      if (p.is_null()) continue;
      for (const auto& q : ends(b)) {
        if (p == q) return p;
      }
    }
    return std::nullopt;
  }

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
};

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
      if (const auto s = shared_end(r[0], r[1])) {
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

  sys_.applySolution();
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

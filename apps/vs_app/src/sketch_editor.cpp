#include "sketch_editor.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>

namespace vs::app {

namespace {

using sketch::ConstraintKind;
using sketch::EntityKind;

constexpr double kAxisSnap = 2.0 * std::numbers::pi / 180.0;  // auto horizontal / vertical

double segment_distance(dvec2 p, dvec2 a, dvec2 b) {
  const dvec2 ab = b - a;
  const double l2 = length_squared(ab);
  double t = l2 > 0 ? dot(p - a, ab) / l2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  return distance(p, a + ab * t);
}

bool is_curve(EntityKind k) { return k != EntityKind::Point; }
bool is_round(EntityKind k) { return k == EntityKind::Circle || k == EntityKind::Arc; }

}  // namespace

std::string_view to_string(SketchTool tool) {
  switch (tool) {
    case SketchTool::Select: return "select";
    case SketchTool::Point: return "point";
    case SketchTool::Line: return "line";
    case SketchTool::Rectangle: return "rectangle";
    case SketchTool::Circle: return "circle";
    case SketchTool::Arc: return "arc";
    case SketchTool::Slot: return "slot";
    case SketchTool::Bezier: return "curve";
  }
  return "?";
}

SketchEditor::SketchEditor(sketch::Sketch sketch, core::ProductSettings settings,
                           feature::Expression::Lookup variables)
    : sketch_(std::move(sketch)),
      settings_(settings),
      variables_(std::move(variables)),
      solver_(sketch::make_solver()) {
  solve();
}

// --- solving -------------------------------------------------------------------------

void SketchEditor::evaluate_dimensions() {
  if (!variables_) return;
  for (const auto& c : sketch_.constraints()) {
    if (!c.dimension || c.dimension->expression.empty()) continue;
    auto v = feature::evaluate_expression(c.dimension->expression, variables_);
    if (v) sketch_.set_dimension(c.id, *v);
  }
}

const sketch::SolveReport& SketchEditor::solve() {
  evaluate_dimensions();
  report_ = solver_->solve(sketch_, settings_);
  return report_;
}

std::vector<std::string> SketchEditor::conflict_labels() const {
  std::vector<std::string> out;
  for (const auto& id : report_.conflicting) out.push_back(sketch_.label(id));
  return out;
}

std::vector<std::string> SketchEditor::redundant_labels() const {
  std::vector<std::string> out;
  for (const auto& id : report_.redundant) out.push_back(sketch_.label(id));
  return out;
}

bool SketchEditor::is_conflicting(const Id& constraint) const {
  return std::find(report_.conflicting.begin(), report_.conflicting.end(), constraint) != report_.conflicting.end();
}

bool SketchEditor::is_free(const Id& entity) const {
  auto free = [&](const Id& id) {
    return std::find(report_.free_entities.begin(), report_.free_entities.end(), id) != report_.free_entities.end();
  };
  if (free(entity)) return true;
  if (const auto* e = sketch_.find_entity(entity)) {
    if (const auto* l = e->as<sketch::Line>()) return free(l->start) || free(l->end);
    if (const auto* a = e->as<sketch::Arc>()) return free(a->center) || free(a->start) || free(a->end);
    if (const auto* c = e->as<sketch::Circle>()) return free(c->center);
    if (const auto* b = e->as<sketch::Bezier>()) {
      return free(b->start) || free(b->control1) || free(b->control2) || free(b->end);
    }
  }
  return false;
}

std::string SketchEditor::status_text() const {
  auto join = [](const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
    return out;
  };
  switch (report_.status) {
    case sketch::SolveStatus::Solved: return "Fully constrained";
    case sketch::SolveStatus::UnderConstrained:
      return fmt::format("Under-constrained: {} degree{} of freedom", report_.dof, report_.dof == 1 ? "" : "s");
    case sketch::SolveStatus::OverConstrained:
      if (!report_.conflicting.empty()) return "Conflicting constraints: " + join(conflict_labels());
      return "Redundant constraints: " + join(redundant_labels());
    case sketch::SolveStatus::Failed: return "The solver didn't converge";
  }
  return {};
}

// --- undo ----------------------------------------------------------------------------

void SketchEditor::record() {
  undo_.push_back(sketch_);
  redo_.clear();
  changed_ = true;
}

bool SketchEditor::undo() {
  if (undo_.empty()) return false;
  redo_.push_back(std::move(sketch_));
  sketch_ = std::move(undo_.back());
  undo_.pop_back();
  finish_tool();
  solve();
  changed_ = true;
  return true;
}

bool SketchEditor::redo() {
  if (redo_.empty()) return false;
  undo_.push_back(std::move(sketch_));
  sketch_ = std::move(redo_.back());
  redo_.pop_back();
  finish_tool();
  solve();
  changed_ = true;
  return true;
}

// --- drawing -------------------------------------------------------------------------

void SketchEditor::set_tool(SketchTool tool) {
  finish_tool();
  tool_ = tool;
}

void SketchEditor::finish_tool() {
  pending_.clear();
  pending_snaps_.clear();
  chain_end_ = {};
}

Id SketchEditor::point_at(dvec2 p, const Id& snap) {
  if (!snap.is_null() && sketch_.point(snap)) return snap;
  return sketch_.add_point(p);
}

std::vector<Id> SketchEditor::click(dvec2 p, const Id& snap) {
  if (tool_ == SketchTool::Select) return {};
  if (!snap.is_null()) {
    if (const auto at = sketch_.point(snap)) p = *at;
  }
  pending_.push_back(p);
  pending_snaps_.push_back(snap);
  // A chain of lines starts each line at the last one's end.
  if ((tool_ == SketchTool::Line || tool_ == SketchTool::Bezier) && !chain_end_.is_null() && pending_.size() == 1) {
    pending_.insert(pending_.begin(), *sketch_.point(chain_end_));
    pending_snaps_.insert(pending_snaps_.begin(), chain_end_);
  }
  std::size_t needed = 0;
  switch (tool_) {
    case SketchTool::Select: return {};
    case SketchTool::Point: needed = 1; break;
    case SketchTool::Line:
    case SketchTool::Rectangle:
    case SketchTool::Circle: needed = 2; break;
    case SketchTool::Arc:
    case SketchTool::Slot: needed = 3; break;
    case SketchTool::Bezier: needed = 4; break;
  }
  if (pending_.size() < needed) return {};
  return finish_shape();
}

std::vector<Id> SketchEditor::finish_shape() {
  // A degenerate shape (zero size) leaves no step and changes nothing,
  // the redo stack included (H5).
  auto saved_redo = std::move(redo_);
  const bool was_changed = changed_;
  record();
  std::vector<Id> added;
  const auto& pts = pending_;
  const auto& snaps = pending_snaps_;
  auto keep = [&](Id id) {
    added.push_back(id);
    return id;
  };
  switch (tool_) {
    case SketchTool::Select: break;
    case SketchTool::Point: keep(point_at(pts[0], snaps[0])); break;
    case SketchTool::Line: {
      if (snaps[0] == snaps[1] && !snaps[0].is_null()) break;
      if (distance(pts[0], pts[1]) <= 0) break;  // a zero-length line
      const Id a = point_at(pts[0], snaps[0]);
      const Id b = point_at(pts[1], snaps[1]);
      if (a == b) break;
      const Id line = keep(sketch_.add_line(a, b));
      const dvec2 d = *sketch_.point(b) - *sketch_.point(a);
      if (std::abs(d.y) <= std::tan(kAxisSnap) * std::abs(d.x)) {
        sketch_.horizontal(line);
      } else if (std::abs(d.x) <= std::tan(kAxisSnap) * std::abs(d.y)) {
        sketch_.vertical(line);
      }
      if (a != snaps[0] || snaps[0].is_null()) added.insert(added.begin(), a);
      if (b != snaps[1] || snaps[1].is_null()) added.push_back(b);
      // The chain goes on from b unless it closed on an existing point.
      chain_end_ = snaps[1].is_null() ? b : Id{};
      break;
    }
    case SketchTool::Rectangle: {
      const dvec2 a = pts[0], c = pts[1];
      if (a.x == c.x || a.y == c.y) break;
      Id p[4];
      p[0] = keep(point_at(a, snaps[0]));
      p[1] = keep(sketch_.add_point({c.x, a.y}));
      p[2] = keep(point_at(c, snaps[1]));
      p[3] = keep(sketch_.add_point({a.x, c.y}));
      Id l[4];
      for (int i = 0; i < 4; ++i) l[i] = keep(sketch_.add_line(p[i], p[(i + 1) % 4]));
      sketch_.horizontal(l[0]);
      sketch_.horizontal(l[2]);
      sketch_.vertical(l[1]);
      sketch_.vertical(l[3]);
      break;
    }
    case SketchTool::Circle: {
      const double r = distance(pts[0], pts[1]);
      if (r <= 0) break;
      const Id c = keep(point_at(pts[0], snaps[0]));
      keep(sketch_.add_circle(c, r));
      break;
    }
    case SketchTool::Arc: {
      const dvec2 c = pts[0];
      const double r = distance(c, pts[1]);
      if (r <= 0 || distance(c, pts[2]) <= 0) break;
      const dvec2 e = c + normalize(pts[2] - c) * r;
      // Start and end in one place: no arc, and no stray points (H5).
      if (!snaps[1].is_null() && snaps[1] == snaps[2]) break;
      if (distance(pts[1], e) <= 0) break;
      const Id center = keep(point_at(c, snaps[0]));
      const Id start = keep(point_at(pts[1], snaps[1]));
      const Id end = keep(point_at(e, snaps[2]));
      if (start == end) break;
      keep(sketch_.add_arc(center, start, end));
      break;
    }
    case SketchTool::Slot: {
      const dvec2 c1 = pts[0], c2 = pts[1];
      if (distance(c1, c2) <= 0) break;
      const dvec2 u = normalize(c2 - c1);
      const dvec2 n{-u.y, u.x};
      const double r = std::abs(dot(pts[2] - c2, n)) > 0 ? std::abs(dot(pts[2] - c2, n)) : distance(pts[2], c2);
      if (r <= 0) break;
      const Id a_c = keep(point_at(c1, snaps[0]));
      const Id b_c = keep(point_at(c2, snaps[1]));
      const Id a1 = keep(sketch_.add_point(c1 + n * r));
      const Id a2 = keep(sketch_.add_point(c1 - n * r));
      const Id b1 = keep(sketch_.add_point(c2 + n * r));
      const Id b2 = keep(sketch_.add_point(c2 - n * r));
      const Id arc_a = keep(sketch_.add_arc(a_c, a1, a2));  // the cap round c1
      const Id arc_b = keep(sketch_.add_arc(b_c, b2, b1));  // the cap round c2
      const Id top = keep(sketch_.add_line(b1, a1));
      const Id bottom = keep(sketch_.add_line(a2, b2));
      sketch_.tangent(top, arc_a);
      sketch_.tangent(top, arc_b);
      sketch_.tangent(bottom, arc_a);
      sketch_.tangent(bottom, arc_b);
      sketch_.equal(arc_a, arc_b);
      break;
    }
    case SketchTool::Bezier: {
      // Ends in one place make a loop, which needs handles apart from them.
      const double reach = distance(pts[0], pts[1]) + distance(pts[1], pts[2]) + distance(pts[2], pts[3]);
      if (reach <= 0) break;
      const bool closed = (!snaps[0].is_null() && snaps[0] == snaps[3]) || pts[0] == pts[3];
      if (closed && (pts[1] == pts[0] || pts[2] == pts[0])) break;
      const Id a = point_at(pts[0], snaps[0]);
      const Id c1 = sketch_.add_point(pts[1]);
      const Id c2 = sketch_.add_point(pts[2]);
      const Id b = closed ? a : point_at(pts[3], snaps[3]);
      if (a != snaps[0] || snaps[0].is_null()) added.push_back(a);
      added.push_back(c1);
      added.push_back(c2);
      if (b != a && (b != snaps[3] || snaps[3].is_null())) added.push_back(b);
      keep(sketch_.add_bezier(a, c1, c2, b));
      chain_end_ = snaps[3].is_null() && !closed ? b : Id{};
      break;
    }
  }
  const Id chain = chain_end_;
  const SketchTool tool = tool_;
  pending_.clear();
  pending_snaps_.clear();
  chain_end_ = tool == SketchTool::Line || tool == SketchTool::Bezier ? chain : Id{};
  if (added.empty()) {
    // Nothing was made (a zero-size shape): drop the empty step.
    sketch_ = std::move(undo_.back());
    undo_.pop_back();
    redo_ = std::move(saved_redo);
    changed_ = was_changed;
    return {};
  }
  solve();
  return added;
}

Id SketchEditor::nearest_point(dvec2 p, double radius) const {
  Id best;
  double best_d = radius;
  for (const auto& e : sketch_.entities()) {
    const auto* pt = e.as<sketch::Point>();
    if (pt == nullptr) continue;
    const double d = distance(pt->position, p);
    if (d <= best_d) {
      best_d = d;
      best = e.id;
    }
  }
  return best;
}

double SketchEditor::distance_to(const Id& entity, dvec2 p) const {
  const auto* e = sketch_.find_entity(entity);
  if (e == nullptr) return std::numeric_limits<double>::infinity();
  switch (e->kind()) {
    case EntityKind::Point: return distance(e->as<sketch::Point>()->position, p);
    case EntityKind::Line: {
      const auto g = sketch::line_geometry(sketch_, entity);
      return g ? segment_distance(p, g->start, g->end) : std::numeric_limits<double>::infinity();
    }
    case EntityKind::Circle: {
      const auto g = sketch::circle_geometry(sketch_, entity);
      return g ? std::abs(distance(p, g->center) - g->radius) : std::numeric_limits<double>::infinity();
    }
    case EntityKind::Arc: {
      const auto g = sketch::arc_geometry(sketch_, entity);
      if (!g) return std::numeric_limits<double>::infinity();
      double a = std::atan2(p.y - g->center.y, p.x - g->center.x);
      const double two_pi = 2.0 * std::numbers::pi;
      while (a < g->start_angle) a += two_pi;
      while (a >= g->start_angle + two_pi) a -= two_pi;
      if (a <= g->end_angle) return std::abs(distance(p, g->center) - g->radius);
      return std::min(distance(p, g->start()), distance(p, g->end()));
    }
    case EntityKind::Bezier: {
      const auto g = sketch::bezier_geometry(sketch_, entity);
      if (!g) return std::numeric_limits<double>::infinity();
      // Nearest chord of a fine polyline: hit-testing needs pixels, not microns.
      const int n = std::min(g->steps_within(1e-3, 32), 1024);
      double best = std::numeric_limits<double>::infinity();
      dvec2 prev = g->at(0.0);
      for (int i = 1; i <= n; ++i) {
        const dvec2 next = g->at(static_cast<double>(i) / n);
        best = std::min(best, segment_distance(p, prev, next));
        prev = next;
      }
      return best;
    }
  }
  return std::numeric_limits<double>::infinity();
}

Id SketchEditor::nearest_entity(dvec2 p, double radius) const {
  if (const Id pt = nearest_point(p, radius); !pt.is_null()) return pt;
  Id best;
  double best_d = radius;
  for (const auto& e : sketch_.entities()) {
    if (!is_curve(e.kind())) continue;
    const double d = distance_to(e.id, p);
    if (d <= best_d) {
      best_d = d;
      best = e.id;
    }
  }
  return best;
}

// --- constraints ---------------------------------------------------------------------

bool SketchEditor::fits(ConstraintKind kind, const std::vector<Id>& refs) const {
  std::vector<EntityKind> k;
  for (const auto& id : refs) {
    const auto* e = sketch_.find_entity(id);
    if (e == nullptr) return false;
    k.push_back(e->kind());
  }
  if (std::set<Id>(refs.begin(), refs.end()).size() != refs.size()) return false;
  const auto P = EntityKind::Point, L = EntityKind::Line;
  const std::size_t n = k.size();
  switch (kind) {
    case ConstraintKind::Coincident: return n == 2 && k[0] == P && k[1] == P;
    case ConstraintKind::PointOnCurve:
      return n == 2 && ((k[0] == P && is_curve(k[1])) || (k[1] == P && is_curve(k[0])));
    case ConstraintKind::Horizontal:
    case ConstraintKind::Vertical: return (n == 1 && k[0] == L) || (n == 2 && k[0] == P && k[1] == P);
    case ConstraintKind::Parallel:
    case ConstraintKind::Perpendicular: return n == 2 && k[0] == L && k[1] == L;
    case ConstraintKind::Tangent:
      return n == 2 && ((k[0] == L && is_round(k[1])) || (is_round(k[0]) && k[1] == L) ||
                        (is_round(k[0]) && is_round(k[1])));
    case ConstraintKind::Equal: return n == 2 && ((k[0] == L && k[1] == L) || (is_round(k[0]) && is_round(k[1])));
    case ConstraintKind::Distance:
      return (n == 1 && k[0] == L) ||
             (n == 2 && ((k[0] == P && k[1] == P) || (k[0] == P && k[1] == L) || (k[0] == L && k[1] == P)));
    case ConstraintKind::HorizontalDistance:
    case ConstraintKind::VerticalDistance: return n == 2 && k[0] == P && k[1] == P;
    case ConstraintKind::Angle: return (n == 1 && k[0] == L) || (n == 2 && k[0] == L && k[1] == L);
    case ConstraintKind::Radius:
    case ConstraintKind::Diameter: return n == 1 && is_round(k[0]);
    case ConstraintKind::Symmetric: return n == 3 && k[0] == P && k[1] == P && (k[2] == L || k[2] == P);
    case ConstraintKind::Fix: return n == 1;
  }
  return false;
}

std::vector<ConstraintKind> SketchEditor::applicable(const std::vector<Id>& refs) const {
  static constexpr ConstraintKind kOrder[] = {
      ConstraintKind::Coincident,    ConstraintKind::PointOnCurve,       ConstraintKind::Horizontal,
      ConstraintKind::Vertical,      ConstraintKind::Parallel,           ConstraintKind::Perpendicular,
      ConstraintKind::Tangent,       ConstraintKind::Equal,              ConstraintKind::Symmetric,
      ConstraintKind::Fix,           ConstraintKind::Distance,           ConstraintKind::HorizontalDistance,
      ConstraintKind::VerticalDistance, ConstraintKind::Angle,           ConstraintKind::Radius,
      ConstraintKind::Diameter};
  std::vector<ConstraintKind> out;
  for (const auto k : kOrder) {
    if (fits(k, refs)) out.push_back(k);
  }
  return out;
}

std::optional<double> SketchEditor::measure(ConstraintKind kind, const std::vector<Id>& refs) const {
  if (!fits(kind, refs)) return std::nullopt;
  auto pos = [&](const Id& id) { return *sketch_.point(id); };
  auto line = [&](const Id& id) { return *sketch::line_geometry(sketch_, id); };
  auto radius = [&](const Id& id) -> double {
    if (const auto c = sketch::circle_geometry(sketch_, id)) return c->radius;
    return sketch::arc_geometry(sketch_, id)->radius;
  };
  auto kind_of = [&](const Id& id) { return sketch_.find_entity(id)->kind(); };
  switch (kind) {
    case ConstraintKind::Distance: {
      if (refs.size() == 1) {
        const auto g = line(refs[0]);
        return distance(g.start, g.end);
      }
      if (kind_of(refs[0]) == EntityKind::Point && kind_of(refs[1]) == EntityKind::Point) {
        return distance(pos(refs[0]), pos(refs[1]));
      }
      const Id p = kind_of(refs[0]) == EntityKind::Point ? refs[0] : refs[1];
      const Id l = kind_of(refs[0]) == EntityKind::Line ? refs[0] : refs[1];
      const auto g = line(l);
      const dvec2 d = normalize(g.end - g.start);
      return std::abs(cross(d, pos(p) - g.start));
    }
    case ConstraintKind::HorizontalDistance: return pos(refs[1]).x - pos(refs[0]).x;
    case ConstraintKind::VerticalDistance: return pos(refs[1]).y - pos(refs[0]).y;
    case ConstraintKind::Angle: {
      const auto a = line(refs[0]);
      const dvec2 da = a.end - a.start;
      if (refs.size() == 1) return std::atan2(da.y, da.x);
      const auto b = line(refs[1]);
      const dvec2 db = b.end - b.start;
      return std::atan2(cross(da, db), dot(da, db));
    }
    case ConstraintKind::Radius: return radius(refs[0]);
    case ConstraintKind::Diameter: return 2.0 * radius(refs[0]);
    default: return std::nullopt;
  }
}

Id SketchEditor::constrain(ConstraintKind kind, std::vector<Id> refs, std::optional<double> value,
                           std::string expression) {
  if (!fits(kind, refs)) return {};
  auto kind_of = [&](const Id& id) { return sketch_.find_entity(id)->kind(); };
  // The documented order: the point first, the line first.
  if (kind == ConstraintKind::PointOnCurve && kind_of(refs[0]) != EntityKind::Point) std::swap(refs[0], refs[1]);
  if (kind == ConstraintKind::Tangent && is_round(kind_of(refs[0])) && kind_of(refs[1]) == EntityKind::Line) {
    std::swap(refs[0], refs[1]);
  }
  if (kind == ConstraintKind::Distance && refs.size() == 2 && kind_of(refs[0]) == EntityKind::Line) {
    std::swap(refs[0], refs[1]);
  }
  std::optional<double> v;
  if (sketch::is_dimensional(kind)) {
    v = value ? value : measure(kind, refs);
    if (!expression.empty() && variables_) {
      if (const auto e = feature::evaluate_expression(expression, variables_)) v = *e;
    }
    if (!v) return {};
  }
  record();
  const Id id = sketch_.add_constraint(kind, std::move(refs), v);
  if (!expression.empty()) sketch_.find_constraint(id)->dimension->expression = std::move(expression);
  solve();
  return id;
}

core::Diagnostics SketchEditor::set_dimension(const Id& constraint, double value, std::string expression) {
  core::Diagnostics d;
  auto* c = sketch_.find_constraint(constraint);
  if (c == nullptr || !c->dimension) {
    d.add(core::Diagnostic::error("app.sketch.not_dimension", "That constraint has no dimension."));
    return d;
  }
  if (!expression.empty()) {
    auto v = feature::evaluate_expression(expression, variables_ ? variables_ : feature::Expression::Lookup(
        [](std::string_view) { return std::optional<double>(); }));
    if (!v) {
      d.add(v.error());
      return d;
    }
    value = *v;
  }
  record();
  c = sketch_.find_constraint(constraint);
  c->dimension->value = value;
  c->dimension->expression = std::move(expression);
  solve();
  return d;
}

// --- other edits ---------------------------------------------------------------------

bool SketchEditor::remove(const std::vector<Id>& ids) {
  std::set<Id> gone;
  std::set<Id> constraints;
  for (const auto& id : ids) {
    if (sketch_.find_entity(id) != nullptr) gone.insert(id);
    if (sketch_.find_constraint(id) != nullptr) constraints.insert(id);
  }
  if (gone.empty() && constraints.empty()) return false;
  auto points_of = [](const sketch::Entity& e) -> std::vector<Id> {
    if (const auto* l = e.as<sketch::Line>()) return {l->start, l->end};
    if (const auto* c = e.as<sketch::Circle>()) return {c->center};
    if (const auto* a = e.as<sketch::Arc>()) return {a->center, a->start, a->end};
    if (const auto* b = e.as<sketch::Bezier>()) return {b->start, b->control1, b->control2, b->end};
    return {};
  };
  // Curves that use a removed point go too.
  for (const auto& e : sketch_.entities()) {
    for (const auto& p : points_of(e)) {
      if (gone.count(p) != 0) gone.insert(e.id);
    }
  }
  // Points that only removed curves used go too (stand-alone points stay).
  std::set<Id> used_by_kept, used_by_gone;
  for (const auto& e : sketch_.entities()) {
    for (const auto& p : points_of(e)) (gone.count(e.id) != 0 ? used_by_gone : used_by_kept).insert(p);
  }
  for (const auto& p : used_by_gone) {
    if (used_by_kept.count(p) == 0) gone.insert(p);
  }
  for (const auto& c : sketch_.constraints()) {
    for (const auto& r : c.refs) {
      if (gone.count(r) != 0) constraints.insert(c.id);
    }
  }
  record();
  for (const auto& c : constraints) sketch_.remove_constraint(c);
  for (const auto& e : gone) sketch_.remove_entity(e);
  finish_tool();
  solve();
  return true;
}

bool SketchEditor::undo_and_forget() {
  if (!undo()) return false;
  redo_.pop_back();
  return true;
}

core::Result<sketch::SvgImport> SketchEditor::import_svg(std::string_view svg,
                                                        const sketch::SvgImportOptions& options) {
  finish_tool();
  sketch::Sketch next = sketch_;
  auto imported = sketch::import_svg(svg, next, options, settings_);
  if (!imported) return imported;
  record();
  sketch_ = std::move(next);
  solve();
  return imported;
}

core::Result<sketch::SketchOffset> SketchEditor::offset(const std::vector<Id>& curves,
                                                        const sketch::OffsetOptions& options) {
  finish_tool();
  sketch::Sketch next = sketch_;
  auto added = sketch::offset_curves(next, curves, options, settings_);
  if (!added) return added;
  record();
  sketch_ = std::move(next);
  solve();
  return added;
}

std::vector<Id> SketchEditor::offset_targets(const std::vector<Id>& ids, bool whole_chains) const {
  if (whole_chains) return sketch::connected_curves(sketch_, ids);
  std::vector<Id> out;
  for (const auto& e : sketch_.entities()) {
    if (e.kind() != sketch::EntityKind::Point && std::find(ids.begin(), ids.end(), e.id) != ids.end()) {
      out.push_back(e.id);
    }
  }
  return out;
}

bool SketchEditor::toggle_construction(const std::vector<Id>& ids) {
  bool any = false;
  for (const auto& id : ids) any = any || sketch_.find_entity(id) != nullptr;
  if (!any) return false;
  record();
  for (const auto& id : ids) {
    if (auto* e = sketch_.find_entity(id)) e->construction = !e->construction;
  }
  solve();
  return true;
}

bool SketchEditor::rename(const Id& id, std::string name) {
  auto* e = sketch_.find_entity(id);
  auto* c = sketch_.find_constraint(id);
  if (e == nullptr && c == nullptr) return false;
  record();
  if (e != nullptr) e->name = std::move(name);
  if (c != nullptr) c->name = std::move(name);
  return true;
}

bool signed_dimension(sketch::ConstraintKind kind) {
  using K = sketch::ConstraintKind;
  return kind == K::HorizontalDistance || kind == K::VerticalDistance || kind == K::Angle;
}

core::Diagnostics SketchEditor::drag_dimension(const Id& constraint, double value, bool record_step) {
  core::Diagnostics d;
  auto* c = sketch_.find_constraint(constraint);
  if (c == nullptr || !c->dimension) {
    d.add(core::Diagnostic::error("app.sketch.not_dimension", "That constraint has no dimension."));
    return d;
  }
  if (!c->dimension->expression.empty()) {
    d.add(core::Diagnostic::error(
        "app.sketch.dimension_driven",
        "'" + sketch_.label(constraint) + "' is driven by '" + c->dimension->expression +
            "'; change that instead (or type a number to drive it by hand)."));
    return d;
  }
  if (!signed_dimension(c->kind)) value = std::max(value, kMinDraggedDimension);
  if (record_step || undo_.empty()) record();
  c = sketch_.find_constraint(constraint);
  c->dimension->value = value;
  solve();
  changed_ = true;
  return d;
}

bool SketchEditor::drag(const Id& point, dvec2 target, bool record_step) {
  if (!sketch_.point(point)) return false;
  if (record_step || undo_.empty()) record();
  evaluate_dimensions();
  report_ = solver_->drag(sketch_, point, target, settings_);
  changed_ = true;
  return report_.applied;
}

}  // namespace vs::app

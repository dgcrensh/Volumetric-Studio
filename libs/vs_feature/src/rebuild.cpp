#include "vs/feature/rebuild.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

#include <fmt/format.h>

#include "vs/core/transform.hpp"
#include "vs/kernel/check.hpp"
#include "vs/kernel/operations.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/sketch/profile.hpp"
#include "vs/sketch/solver.hpp"

namespace vs::feature {

namespace {

using core::cross;
using core::dot;
using core::dtransform;
using core::length;
using core::normalize;
using core::SubjectRef;
using kernel::Axis;
using kernel::Face;
using kernel::Plane;
using kernel::Solid;
using sketch::SketchPlane;

constexpr double kTwoPi = 2.0 * std::numbers::pi;
// Two unit vectors are parallel when |dot| is within this of 1.
constexpr double kParallel = 1e-9;

bool parallel(const dvec3& a, const dvec3& b) { return 1.0 - std::abs(dot(a, b)) <= kParallel; }

// Builds one feature's result. Holds the context every feature needs.
class Builder {
 public:
  Builder(const FeatureTree& tree, const ProductSettings& settings, RebuildResult& out,
          std::size_t index)
      : tree_(tree),
        settings_(settings),
        out_(out),
        index_(index),
        feature_(tree.features()[index]),
        self_(tree.subject(feature_.id)),
        body_(index == 0 ? Solid{} : out.features[index - 1].body) {}

  FeatureResult run() {
    result_.feature = feature_.id;
    result_.body = body_;
    if (feature_.suppressed) {
      result_.status = FeatureStatus::Suppressed;
      return std::move(result_);
    }
    const Diagnostics problems = std::visit([this](const auto& def) { return build(def); },
                                            feature_.definition);
    result_.diagnostics.append(problems);
    if (problems.has_errors()) {
      result_.status = FeatureStatus::Failed;
      result_.body = body_;
      result_.plane.reset();
      result_.axis.reset();
      result_.point.reset();
      result_.sketch.reset();
      result_.faces.clear();
    }
    return std::move(result_);
  }

 private:
  const FeatureTree& tree_;
  const ProductSettings& settings_;
  RebuildResult& out_;
  std::size_t index_;
  const Feature& feature_;
  SubjectRef self_;
  Solid body_;  // as the features above left it
  FeatureResult result_;

  // --- diagnostics ---------------------------------------------------------

  Diagnostic error(std::string code, std::string message, std::vector<SubjectRef> more = {}) {
    std::vector<SubjectRef> subjects{self_};
    subjects.insert(subjects.end(), more.begin(), more.end());
    return Diagnostic::error(std::move(code), fmt::format("'{}': {}", self_.label, message),
                             std::move(subjects));
  }

  // Another module's diagnostic, re-pointed at this feature.
  Diagnostic wrap(Diagnostic d) {
    d.message = fmt::format("'{}': {}", self_.label, d.message);
    d.subjects.insert(d.subjects.begin(), self_);
    return d;
  }

  static Diagnostics one(Diagnostic d) {
    Diagnostics ds;
    ds.add(std::move(d));
    return ds;
  }

  // --- parameters ----------------------------------------------------------

  Result<double> eval(const Param& p, std::string_view what) {
    if (p.expression.empty()) {
      if (!std::isfinite(p.value)) {
        return error("feature.expression.not_finite", fmt::format("the {} isn't a finite number", what));
      }
      return p.value;
    }
    auto e = Expression::parse(p.expression);
    if (!e) {
      Diagnostic d = e.error();
      d.message = fmt::format("the {}: {}", what, d.message);
      return wrap(std::move(d));
    }
    for (const auto& name : e->references()) {
      if (out_.variables.get(name)) continue;
      if (const Variable* v = tree_.variables().find(name)) {
        return error("feature.variable.upstream_failed",
                     fmt::format("the {} uses variable '{}', which has no value", what, name),
                     {tree_.variables().subject(v->id)});
      }
      return error("feature.variable.unknown",
                   fmt::format("the {} uses '{}', which isn't a variable", what, name));
    }
    auto v = e->evaluate(out_.variables.lookup());
    if (!v) {
      Diagnostic d = v.error();
      d.message = fmt::format("the {}: {}", what, d.message);
      return wrap(std::move(d));
    }
    return *v;
  }

  // --- references ----------------------------------------------------------

  Result<const FeatureResult*> use(const Id& id, std::string_view what) {
    const auto at = tree_.index_of(id);
    if (!at) {
      return error("feature.reference.broken",
                   fmt::format("the {} it uses no longer exists", what));
    }
    const SubjectRef other = tree_.subject(id);
    if (*at >= index_) {
      return error("feature.reference.forward",
                   fmt::format("it uses '{}', which comes after it; move '{}' above it",
                               other.label, other.label),
                   {other});
    }
    const FeatureResult& r = out_.features[*at];
    if (r.status == FeatureStatus::Suppressed) {
      return error("feature.reference.suppressed",
                   fmt::format("it uses '{}', which is suppressed", other.label), {other});
    }
    if (r.status == FeatureStatus::Failed) {
      return error("feature.reference.upstream_failed",
                   fmt::format("it uses '{}', which failed to rebuild", other.label), {other});
    }
    return &r;
  }

  Result<const FeatureResult*> use_sketch(const Id& id) {
    auto r = use(id, "sketch");
    if (!r) return r;
    if (!(*r)->sketch) {
      const SubjectRef other = tree_.subject(id);
      return error("feature.reference.wrong_kind",
                   fmt::format("'{}' is not a sketch", other.label), {other});
    }
    return r;
  }

  Result<SketchPlane> plane_of(const PlaneRef& ref) {
    if (const auto* p = std::get_if<PrincipalPlane>(&ref)) return principal_frame(*p);
    if (const auto* f = std::get_if<FeatureRef>(&ref)) {
      auto r = use(f->feature, "plane");
      if (!r) return r.error();
      if (!(*r)->plane) {
        const SubjectRef other = tree_.subject(f->feature);
        return error("feature.reference.wrong_kind",
                     fmt::format("'{}' is not a plane or a sketch", other.label), {other});
      }
      return *(*r)->plane;
    }
    const auto& face = std::get<FaceRef>(ref);
    auto r = use(face.feature, "face's feature");
    if (!r) return r.error();
    const SubjectRef other = tree_.subject(face.feature);
    const auto named = std::find_if((*r)->faces.begin(), (*r)->faces.end(),
                                    [&](const NamedFace& n) { return n.name == face.name; });
    if (named == (*r)->faces.end()) {
      std::string why = fmt::format("the face '{}' of '{}' no longer exists", face.name,
                                    other.label);
      if (face.name.starts_with("side:")) {
        why += "; the sketch line that made it was probably deleted or is no longer part of a "
               "profile";
      }
      return error("feature.reference.broken", why, {other});
    }
    // Find it again on the body as this feature sees it.
    const Plane& want = named->plane;
    for (const Face& f : kernel::faces(body_)) {
      const auto fp = kernel::face_plane(f);
      if (!fp) continue;
      if (dot(fp->normal, want.normal) < 1.0 - kParallel) continue;
      if (std::abs(dot(fp->origin - want.origin, want.normal)) > settings_.linear_tolerance_mm) {
        continue;
      }
      return frame_for(want);
    }
    return error("feature.reference.broken",
                 fmt::format("the face '{}' of '{}' is no longer on the body; a later feature "
                             "removed it",
                             face.name, other.label),
                 {other});
  }

  Result<Axis> axis_of(const AxisRef& ref) {
    if (const auto* p = std::get_if<PrincipalAxis>(&ref)) {
      switch (*p) {
        case PrincipalAxis::X: return Axis{{0, 0, 0}, {1, 0, 0}};
        case PrincipalAxis::Y: return Axis{{0, 0, 0}, {0, 1, 0}};
        case PrincipalAxis::Z: return Axis{{0, 0, 0}, {0, 0, 1}};
      }
    }
    if (const auto* f = std::get_if<FeatureRef>(&ref)) {
      auto r = use(f->feature, "axis");
      if (!r) return r.error();
      if (!(*r)->axis) {
        const SubjectRef other = tree_.subject(f->feature);
        return error("feature.reference.wrong_kind",
                     fmt::format("'{}' is not an axis", other.label), {other});
      }
      return *(*r)->axis;
    }
    const auto& e = std::get<SketchEntityRef>(ref);
    auto r = use_sketch(e.sketch);
    if (!r) return r.error();
    const sketch::Sketch& sk = *(*r)->sketch;
    const SubjectRef other = tree_.subject(e.sketch);
    const auto line = sketch::line_geometry(sk, e.entity);
    if (!line) {
      if (sk.find_entity(e.entity)) {
        return error("feature.reference.wrong_kind",
                     fmt::format("the axis '{}' in '{}' is not a line", sk.label(e.entity),
                                 other.label),
                     {other});
      }
      return error("feature.reference.broken",
                   fmt::format("the sketch line it uses as an axis no longer exists in '{}'",
                               other.label),
                   {other});
    }
    const dvec3 a = sk.plane().to_model(line->start);
    const dvec3 b = sk.plane().to_model(line->end);
    if (length(b - a) < settings_.min_feature_size_mm()) {
      return error("feature.datum.degenerate",
                   fmt::format("the line '{}' in '{}' has no length", sk.label(e.entity),
                               other.label),
                   {other});
    }
    return Axis{a, normalize(b - a)};
  }

  Result<dvec3> point_of(const PointRef& ref) {
    if (const auto* f = std::get_if<FeatureRef>(&ref)) {
      auto r = use(f->feature, "point");
      if (!r) return r.error();
      if (!(*r)->point) {
        const SubjectRef other = tree_.subject(f->feature);
        return error("feature.reference.wrong_kind",
                     fmt::format("'{}' is not a point", other.label), {other});
      }
      return *(*r)->point;
    }
    const auto& e = std::get<SketchEntityRef>(ref);
    auto r = use_sketch(e.sketch);
    if (!r) return r.error();
    const sketch::Sketch& sk = *(*r)->sketch;
    const SubjectRef other = tree_.subject(e.sketch);
    const auto p = sk.point(e.entity);
    if (!p) {
      if (sk.find_entity(e.entity)) {
        return error("feature.reference.wrong_kind",
                     fmt::format("'{}' in '{}' is not a point", sk.label(e.entity), other.label),
                     {other});
      }
      return error("feature.reference.broken",
                   fmt::format("the sketch point it uses no longer exists in '{}'", other.label),
                   {other});
    }
    return sk.plane().to_model(*p);
  }

  // --- the body ------------------------------------------------------------

  kernel::BooleanOptions fuzzy() const { return {settings_.linear_tolerance_mm}; }

  Result<Solid> combine(const Solid& tool, Operation op) {
    if (op == Operation::Add) {
      if (body_.is_empty()) return tool;
      auto r = kernel::unite(body_, tool, fuzzy());
      if (!r) return wrap(r.error());
      return r;
    }
    if (body_.is_empty()) {
      return error("feature.cut.no_body",
                   "there is no body to cut yet; add material with a feature above it first");
    }
    auto r = kernel::cut(body_, tool, fuzzy());
    if (!r) return wrap(r.error());
    return r;
  }

  void set_body(Solid body) {
    result_.body = std::move(body);
    const auto audit = kernel::audit_tolerances(result_.body, settings_);
    result_.max_tolerance = audit.max_tolerance();
    for (const auto& d : audit.diagnostics()) {
      Diagnostic w = wrap(d);
      w.severity = core::Severity::Warning;
      result_.diagnostics.add(std::move(w));
    }
  }

  struct Profiles {
    std::vector<Face> faces;
    std::vector<Id> lines;  // the line entities along the profiles' loops
  };

  Result<Profiles> profiles_of(const sketch::Sketch& sk, const SubjectRef& sketch_subject) {
    auto profiles = sketch::extract_profiles(sk, settings_);
    if (!profiles) return wrap(profiles.error());
    if (profiles->empty()) {
      return error("feature.sketch.no_profile",
                   fmt::format("'{}' has no closed profile", sketch_subject.label),
                   {sketch_subject});
    }
    Profiles out;
    for (const auto& p : *profiles) {
      auto face = sketch::make_face(sk, p, settings_);
      if (!face) return wrap(face.error());
      out.faces.push_back(*face);
      auto add_lines = [&](const sketch::ProfileLoop& loop) {
        for (const auto& e : loop.edges) {
          const auto* entity = sk.find_entity(e.entity);
          if (entity && entity->kind() == sketch::EntityKind::Line) out.lines.push_back(e.entity);
        }
      };
      add_lines(p.outer);
      for (const auto& h : p.holes) add_lines(h);
    }
    return out;
  }

  // Names the side faces of `tool` made by the sketch's lines.
  void name_sides(const Solid& tool, const sketch::Sketch& sk, const std::vector<Id>& lines,
                  const dvec3& dir, Operation op) {
    const auto tool_faces = kernel::faces(tool);
    std::vector<std::optional<Plane>> planes;
    planes.reserve(tool_faces.size());
    for (const Face& f : tool_faces) planes.push_back(kernel::face_plane(f));
    const double tol = settings_.linear_tolerance_mm;
    for (const Id& id : lines) {
      const auto line = sketch::line_geometry(sk, id);
      if (!line) continue;
      const dvec3 a = sk.plane().to_model(line->start);
      const dvec3 b = sk.plane().to_model(line->end);
      for (const auto& fp : planes) {
        if (!fp || parallel(fp->normal, dir)) continue;
        if (std::abs(dot(a - fp->origin, fp->normal)) > tol ||
            std::abs(dot(b - fp->origin, fp->normal)) > tol) {
          continue;
        }
        const dvec3 normal = op == Operation::Add ? fp->normal : -fp->normal;
        result_.faces.push_back({"side:" + id.to_string(), Plane{(a + b) * 0.5, normal}});
        break;
      }
    }
  }

  // --- features ------------------------------------------------------------

  Diagnostics build(const SketchFeature& sf) {
    auto plane = plane_of(sf.plane);
    if (!plane) return one(plane.error());
    auto sk = std::make_shared<sketch::Sketch>(sf.sketch);
    sk->set_plane(*plane);
    for (const auto& c : sf.sketch.constraints()) {
      if (!c.dimension || c.dimension->expression.empty()) continue;
      auto v = eval(Param::expr(c.dimension->expression),
                    fmt::format("dimension '{}'", sf.sketch.label(c.id)));
      if (!v) {
        Diagnostic d = v.error();
        d.subjects.insert(d.subjects.begin() + 1, sf.sketch.subject(c.id));
        return one(std::move(d));
      }
      sk->set_dimension(c.id, *v);
    }
    const auto report = sketch::solve(*sk, settings_);
    if (!report.ok()) {
      Diagnostics ds;
      const std::string why =
          report.diagnostics.empty() ? std::string(sketch::to_string(report.status))
                                     : report.diagnostics[0].message;
      ds.add(error("feature.sketch.unsolved", fmt::format("the sketch doesn't solve: {}", why)));
      for (const auto& d : report.diagnostics) ds.add(wrap(d));
      return ds;
    }
    result_.plane = *plane;
    result_.sketch = std::move(sk);
    return {};
  }

  Diagnostics build(const DatumPlaneFeature& dp) {
    auto plane = std::visit([this](const auto& def) { return datum_plane(def); }, dp.definition);
    if (!plane) return one(plane.error());
    result_.plane = *plane;
    return {};
  }

  Result<SketchPlane> datum_plane(const PlaneOffset& def) {
    auto base = plane_of(def.base);
    if (!base) return base;
    auto d = eval(def.distance, "offset");
    if (!d) return d.error();
    SketchPlane p = *base;
    p.origin = p.origin + p.normal() * *d;
    return p;
  }

  Result<SketchPlane> datum_plane(const PlaneThroughPoints& def) {
    auto a = point_of(def.a);
    if (!a) return a.error();
    auto b = point_of(def.b);
    if (!b) return b.error();
    auto c = point_of(def.c);
    if (!c) return c.error();
    const dvec3 u = *b - *a;
    const dvec3 v = *c - *a;
    const dvec3 n = cross(u, v);
    if (length(u) < settings_.min_feature_size_mm() ||
        length(n) < settings_.min_feature_size_mm() * length(u)) {
      return error("feature.datum.degenerate",
                   "its three points are coincident or in a line, so they don't fix a plane");
    }
    SketchPlane p;
    p.origin = *a;
    p.x_axis = normalize(u);
    const dvec3 normal = normalize(n);
    p.y_axis = normalize(cross(normal, p.x_axis));
    return p;
  }

  Result<SketchPlane> datum_plane(const PlaneAxisAngle& def) {
    auto axis = axis_of(def.axis);
    if (!axis) return axis.error();
    auto base = plane_of(def.base);
    if (!base) return base.error();
    auto angle = eval(def.angle, "angle");
    if (!angle) return angle.error();
    const dvec3 d = normalize(axis->direction);
    const dvec3 nb = base->normal();
    const dvec3 perp = nb - d * dot(nb, d);
    if (length(perp) < 1e-9) {
      return error("feature.datum.degenerate",
                   "its axis is perpendicular to the base plane, so the angle has no meaning");
    }
    const dvec3 normal = dtransform::rotate(d, *angle).apply_vector(normalize(perp));
    SketchPlane p;
    p.origin = axis->origin;
    p.x_axis = d;
    p.y_axis = normalize(cross(normal, d));
    return p;
  }

  Diagnostics build(const DatumAxisFeature& da) {
    auto axis = std::visit([this](const auto& def) { return datum_axis(def); }, da.definition);
    if (!axis) return one(axis.error());
    result_.axis = Axis{axis->origin, normalize(axis->direction)};
    return {};
  }

  Result<Axis> datum_axis(const AxisThroughPoints& def) {
    auto a = point_of(def.a);
    if (!a) return a.error();
    auto b = point_of(def.b);
    if (!b) return b.error();
    if (length(*b - *a) < settings_.min_feature_size_mm()) {
      return error("feature.datum.degenerate",
                   "its two points coincide, so they don't fix a direction");
    }
    return Axis{*a, normalize(*b - *a)};
  }

  Result<Axis> datum_axis(const AxisPlaneIntersection& def) {
    auto a = plane_of(def.a);
    if (!a) return a.error();
    auto b = plane_of(def.b);
    if (!b) return b.error();
    const dvec3 na = a->normal();
    const dvec3 nb = b->normal();
    const dvec3 c = cross(na, nb);
    const double c2 = dot(c, c);
    if (c2 < 1e-18) {
      return error("feature.datum.degenerate", "its two planes are parallel, so they don't meet");
    }
    const double da = dot(na, a->origin);
    const double db = dot(nb, b->origin);
    // The point of the line closest to the model origin.
    const dvec3 origin = cross(nb * da - na * db, c) / c2;
    return Axis{origin, normalize(c)};
  }

  Result<Axis> datum_axis(const AxisAlong& def) { return axis_of(def.axis); }

  Diagnostics build(const DatumPointFeature& dp) {
    auto point = std::visit([this](const auto& def) { return datum_point(def); }, dp.definition);
    if (!point) return one(point.error());
    result_.point = *point;
    return {};
  }

  Result<dvec3> datum_point(const PointAt& def) {
    auto x = eval(def.x, "x coordinate");
    if (!x) return x.error();
    auto y = eval(def.y, "y coordinate");
    if (!y) return y.error();
    auto z = eval(def.z, "z coordinate");
    if (!z) return z.error();
    return dvec3{*x, *y, *z};
  }

  Result<dvec3> datum_point(const PointAtRef& def) { return point_of(def.point); }

  Result<dvec3> datum_point(const PointAxisPlane& def) {
    auto axis = axis_of(def.axis);
    if (!axis) return axis.error();
    auto plane = plane_of(def.plane);
    if (!plane) return plane.error();
    const dvec3 n = plane->normal();
    const dvec3 d = normalize(axis->direction);
    const double denom = dot(d, n);
    if (std::abs(denom) < 1e-9) {
      return error("feature.datum.degenerate",
                   "its axis is parallel to its plane, so they don't meet in a point");
    }
    const double t = dot(plane->origin - axis->origin, n) / denom;
    return axis->origin + d * t;
  }

  Diagnostics build(const ExtrudeFeature& ex) {
    auto sr = use_sketch(ex.sketch);
    if (!sr) return one(sr.error());
    const sketch::Sketch& sk = *(*sr)->sketch;
    const SubjectRef sketch_subject = tree_.subject(ex.sketch);
    auto profiles = profiles_of(sk, sketch_subject);
    if (!profiles) return one(profiles.error());

    const SketchPlane& plane = sk.plane();
    dvec3 dir = ex.operation == Operation::Add ? plane.normal() : -plane.normal();
    if (ex.reverse) dir = -dir;

    auto draft = eval(ex.draft, "draft angle");
    if (!draft) return one(draft.error());

    double depth = 0.0;
    double start = 0.0;  // offset of the start face from the sketch plane, along dir
    switch (ex.extent) {
      case Extent::Blind:
      case Extent::Symmetric: {
        auto d = eval(ex.depth, "depth");
        if (!d) return one(d.error());
        if (*d < settings_.min_feature_size_mm()) {
          return one(error("feature.extrude.bad_depth",
                           fmt::format("the depth ({} mm) must be at least the minimum feature "
                                       "size ({} mm); use 'reverse' to go the other way",
                                       *d, settings_.min_feature_size_mm())));
        }
        depth = *d;
        if (ex.extent == Extent::Symmetric) start = -depth / 2;
        break;
      }
      case Extent::ThroughAll: {
        if (body_.is_empty()) {
          return one(error("feature.extrude.through_all_needs_body",
                           "'through all' needs a body to go through; give it a depth instead"));
        }
        const auto box = kernel::bounding_box(body_);
        double far = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < 8; ++i) {
          const dvec3 corner{(i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y,
                             (i & 4) ? box.max.z : box.min.z};
          far = std::max(far, dot(corner - plane.origin, dir));
        }
        if (far < settings_.min_feature_size_mm()) {
          return one(error("feature.extrude.nothing_ahead",
                           "the body lies entirely behind the sketch in the extrusion's "
                           "direction; reverse the direction"));
        }
        // Go a millimetre past the body, but not out of the working space.
        depth = far + 1.0;
        for (const Face& f : profiles->faces) {
          const auto fb = kernel::bounding_box(f);
          const double e = settings_.working_half_extent_mm;
          for (std::size_t k = 0; k < 3; ++k) {
            if (std::abs(dir[k]) < 1e-12) continue;
            const double from = dir[k] > 0 ? fb.max[k] : fb.min[k];
            const double room = ((dir[k] > 0 ? e : -e) - from) / dir[k];
            depth = std::min(depth, room);
          }
        }
        depth = std::max(depth, far);
        break;
      }
      case Extent::UpToPlane: {
        auto target = plane_of(ex.up_to);
        if (!target) return one(target.error());
        if (!parallel(target->normal(), dir)) {
          return one(error("feature.extrude.up_to_not_parallel",
                           "the plane to extrude up to must be parallel to the sketch plane"));
        }
        depth = dot(target->origin - plane.origin, dir);
        if (depth < settings_.min_feature_size_mm()) {
          return one(error("feature.extrude.up_to_behind",
                           "the plane to extrude up to is behind the sketch (or on it); "
                           "reverse the direction"));
        }
        break;
      }
    }

    // One solid per profile, united into the tool.
    Solid tool;
    for (const Face& face : profiles->faces) {
      Result<Solid> piece = Solid{};
      if (ex.extent == Extent::Symmetric && *draft != 0.0) {
        // Taper both ways from the sketch plane.
        auto up = kernel::make_drafted_prism(face, dir * (depth / 2), *draft, settings_);
        if (!up) return one(wrap(up.error()));
        auto down = kernel::make_drafted_prism(face, -dir * (depth / 2), *draft, settings_);
        if (!down) return one(wrap(down.error()));
        piece = kernel::unite(*up, *down, fuzzy());
      } else {
        Face f = face;
        if (start != 0.0) {
          auto moved = kernel::transformed(face, dtransform::translate(dir * start));
          if (!moved) return one(wrap(moved.error()));
          auto as_face = Face::from(*moved);
          if (!as_face) return one(wrap(as_face.error()));
          f = *as_face;
        }
        piece = kernel::make_drafted_prism(f, dir * depth, *draft, settings_);
      }
      if (!piece) return one(wrap(piece.error()));
      if (tool.is_empty()) {
        tool = *piece;
      } else {
        auto merged = kernel::unite(tool, *piece, fuzzy());
        if (!merged) return one(wrap(merged.error()));
        tool = *merged;
      }
    }

    auto body = combine(tool, ex.operation);
    if (!body) return one(body.error());
    set_body(*body);

    const dvec3 start_point = plane.origin + dir * start;
    const dvec3 end_point = start_point + dir * depth;
    if (ex.operation == Operation::Add) {
      result_.faces.push_back({"start", Plane{start_point, -dir}});
      result_.faces.push_back({"end", Plane{end_point, dir}});
    } else if (ex.extent == Extent::Blind || ex.extent == Extent::UpToPlane) {
      result_.faces.push_back({"end", Plane{end_point, -dir}});
    }
    name_sides(tool, sk, profiles->lines, dir, ex.operation);
    return {};
  }

  Diagnostics build(const RevolveFeature& rv) {
    auto sr = use_sketch(rv.sketch);
    if (!sr) return one(sr.error());
    const sketch::Sketch& sk = *(*sr)->sketch;
    const SubjectRef sketch_subject = tree_.subject(rv.sketch);
    auto axis = axis_of(rv.axis);
    if (!axis) return one(axis.error());
    auto angle = eval(rv.angle, "angle");
    if (!angle) return one(angle.error());
    if (*angle <= 0.0 || *angle > kTwoPi + 1e-9) {
      return one(error("feature.revolve.bad_angle",
                       fmt::format("the angle ({:.6g}°) must be above 0 and at most 360°",
                                   *angle * 180.0 / std::numbers::pi)));
    }
    const double turn = std::min(*angle, kTwoPi);
    const SketchPlane& plane = sk.plane();
    const dvec3 n = plane.normal();
    if (std::abs(dot(axis->direction, n)) > 1e-9 ||
        std::abs(dot(axis->origin - plane.origin, n)) > settings_.linear_tolerance_mm) {
      return one(error("feature.revolve.axis_not_in_plane",
                       fmt::format("the axis must lie in the plane of '{}'",
                                   sketch_subject.label),
                       {sketch_subject}));
    }
    auto profiles = profiles_of(sk, sketch_subject);
    if (!profiles) return one(profiles.error());

    Axis ax{axis->origin, rv.reverse ? -axis->direction : axis->direction};
    Solid tool;
    for (const Face& face : profiles->faces) {
      auto piece = kernel::make_revolution(face, ax, turn, settings_);
      if (!piece) return one(wrap(piece.error()));
      if (tool.is_empty()) {
        tool = *piece;
      } else {
        auto merged = kernel::unite(tool, *piece, fuzzy());
        if (!merged) return one(wrap(merged.error()));
        tool = *merged;
      }
    }
    auto body = combine(tool, rv.operation);
    if (!body) return one(body.error());
    set_body(*body);

    if (turn < kTwoPi - 1e-9) {
      // The material leaves the sketch plane along `sweep`, so the start face
      // faces the other way and the end face, turned by the angle, faces on.
      const auto centre = kernel::face_plane(profiles->faces.front());
      const dvec3 c = centre ? centre->origin : plane.origin;
      const dvec3 sweep = cross(ax.direction, c - ax.origin);
      const dvec3 start_normal = dot(sweep, n) >= 0 ? -n : n;
      const dtransform r = dtransform::translate(ax.origin) *
                           dtransform::rotate(ax.direction, turn) *
                           dtransform::translate(-ax.origin);
      const dvec3 end_normal = -r.apply_vector(start_normal);
      const double s = rv.operation == Operation::Add ? 1.0 : -1.0;
      result_.faces.push_back({"start", Plane{c, start_normal * s}});
      result_.faces.push_back({"end", Plane{r.apply_point(c), end_normal * s}});
    }
    return {};
  }
};

}  // namespace

std::string_view to_string(FeatureStatus status) {
  switch (status) {
    case FeatureStatus::Ok: return "ok";
    case FeatureStatus::Failed: return "failed";
    case FeatureStatus::Suppressed: return "suppressed";
  }
  return "ok";
}

const FeatureResult* RebuildResult::find(const Id& feature) const {
  for (const auto& r : features) {
    if (r.feature == feature) return &r;
  }
  return nullptr;
}

std::size_t RebuildResult::rebuilt_count() const {
  return static_cast<std::size_t>(std::count_if(features.begin(), features.end(), [](const auto& r) {
    return !r.reused && r.status != FeatureStatus::Suppressed;
  }));
}

sketch::SketchPlane principal_frame(PrincipalPlane plane) {
  switch (plane) {
    case PrincipalPlane::XY: return {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    case PrincipalPlane::YZ: return {{0, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    case PrincipalPlane::ZX: return {{0, 0, 0}, {0, 0, 1}, {1, 0, 0}};
  }
  return {};
}

sketch::SketchPlane frame_for(const kernel::Plane& plane) {
  const dvec3 n = normalize(plane.normal);
  const std::array<dvec3, 3> axes{dvec3{1, 0, 0}, dvec3{0, 1, 0}, dvec3{0, 0, 1}};
  std::size_t best = 0;
  for (std::size_t i = 1; i < 3; ++i) {
    if (std::abs(dot(axes[i], n)) < std::abs(dot(axes[best], n)) - 1e-12) best = i;
  }
  sketch::SketchPlane p;
  p.origin = n * dot(plane.origin, n);
  p.x_axis = normalize(axes[best] - n * dot(axes[best], n));
  p.y_axis = normalize(cross(n, p.x_axis));
  return p;
}

RebuildResult rebuild(const FeatureTree& tree, const ProductSettings& settings,
                      const RebuildOptions& options) {
  RebuildResult out;
  out.variables = tree.variables().evaluate();
  out.diagnostics.append(out.variables.diagnostics);
  const auto& features = tree.features();
  out.features.reserve(features.size());

  std::size_t first = 0;
  if (options.previous && !options.start.is_null()) {
    const RebuildResult& prev = *options.previous;
    const auto k = tree.index_of(options.start);
    bool reusable = k.has_value() && prev.variables == out.variables &&
                    prev.features.size() >= *k;
    for (std::size_t i = 0; reusable && i < *k; ++i) {
      reusable = prev.features[i].feature == features[i].id &&
                 (prev.features[i].status == FeatureStatus::Suppressed) == features[i].suppressed;
    }
    if (reusable) {
      first = *k;
      for (std::size_t i = 0; i < first; ++i) {
        out.features.push_back(prev.features[i]);
        out.features.back().reused = true;
      }
    }
  }

  for (std::size_t i = first; i < features.size(); ++i) {
    Builder builder(tree, settings, out, i);
    out.features.push_back(builder.run());
  }
  for (const auto& r : out.features) out.diagnostics.append(r.diagnostics);
  if (!out.features.empty()) out.body = out.features.back().body;
  return out;
}

}  // namespace vs::feature

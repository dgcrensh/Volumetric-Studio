#include "vs/feature/rebuild.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>

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
using core::dvec2;
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

// The most copies a pattern may make, to keep a typo from hanging a rebuild.
constexpr long long kMaxPatternCount = 1000;

// True when `face` lies on the surface `named` describes, facing the same way.
bool on_named(const NamedFace& named, const Face& face, double tol) {
  if (named.cylinder) {
    const auto c = kernel::face_cylinder(face);
    if (!c || c->inward != named.cylinder->inward) return false;
    if (std::abs(c->radius - named.cylinder->radius) > tol) return false;
    const dvec3 d = normalize(named.cylinder->axis.direction);
    if (!parallel(normalize(c->axis.direction), d)) return false;
    const dvec3 off = c->axis.origin - named.cylinder->axis.origin;
    return length(off - d * dot(off, d)) <= tol;
  }
  const auto fp = kernel::face_plane(face);
  if (!fp) return false;
  if (dot(fp->normal, named.plane.normal) < 1.0 - kParallel) return false;
  return std::abs(dot(fp->origin - named.plane.origin, named.plane.normal)) <= tol;
}

NamedFace transformed(const NamedFace& face, const dtransform& t, std::string name) {
  NamedFace out{std::move(name), {}, std::nullopt};
  out.plane = Plane{t.apply_point(face.plane.origin), normalize(t.apply_vector(face.plane.normal))};
  if (face.cylinder) {
    kernel::Cylinder c = *face.cylinder;
    c.axis = Axis{t.apply_point(c.axis.origin), normalize(t.apply_vector(c.axis.direction))};
    out.cylinder = c;
  }
  return out;
}

// Adds `edge` to `edges` unless it is already there.
void add_unique(std::vector<kernel::Edge>& edges, const kernel::Edge& edge) {
  for (const auto& e : edges) {
    if (e.is_same(edge)) return;
  }
  edges.push_back(edge);
}

// --- paths ------------------------------------------------------------------

dvec3 seg_start(const kernel::ProfileSegment& s) {
  return std::visit([](const auto& seg) { return seg.start; }, s);
}
dvec3 seg_end(const kernel::ProfileSegment& s) {
  return std::visit([](const auto& seg) { return seg.end; }, s);
}

kernel::ProfileSegment reversed(const kernel::ProfileSegment& s) {
  if (const auto* line = std::get_if<kernel::LineSegment>(&s)) {
    return kernel::LineSegment{line->end, line->start};
  }
  const auto& arc = std::get<kernel::ArcSegment>(s);
  return kernel::ArcSegment{arc.end, arc.through, arc.start};
}

// The unit tangent of a segment at its start (at_end = false) or its end,
// in the direction of travel.
dvec3 tangent(const kernel::ProfileSegment& s, bool at_end) {
  if (const auto* line = std::get_if<kernel::LineSegment>(&s)) {
    return normalize(line->end - line->start);
  }
  const auto& arc = std::get<kernel::ArcSegment>(s);
  // The circle through the three points: centre from the standard formula.
  const dvec3 a = arc.start - arc.end;
  const dvec3 b = arc.through - arc.end;
  const dvec3 axb = cross(a, b);
  const dvec3 centre =
      arc.end + cross(b * dot(a, a) - a * dot(b, b), axb) / (2.0 * dot(axb, axb));
  // Travel runs counter-clockwise about n = (through - start) x (end - through).
  const dvec3 n = normalize(cross(arc.through - arc.start, arc.end - arc.through));
  const dvec3 p = at_end ? arc.end : arc.start;
  return normalize(cross(n, p - centre));
}

// Builds one feature's result. Holds the context every feature needs.
class Builder {
 public:
  Builder(const FeatureTree& tree, const ProductSettings& settings, RebuildResult& out,
          std::size_t index, const ExternalInputs* inputs)
      : tree_(tree),
        inputs_(inputs),
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
      result_.tool = Solid{};
      result_.operation.reset();
      result_.max_tolerance = 0.0;
    }
    return std::move(result_);
  }

 private:
  const FeatureTree& tree_;
  const ExternalInputs* inputs_;
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

  // An input from outside the tree, when `id` names one and no feature.
  const ExternalInput* external(const Id& id) const {
    if (!inputs_ || tree_.index_of(id)) return nullptr;
    return inputs_->find(id);
  }

  SubjectRef subject_of(const Id& id) const {
    if (const ExternalInput* in = external(id)) return in->subject;
    return tree_.subject(id);
  }

  Result<const FeatureResult*> use(const Id& id, std::string_view what) {
    if (const ExternalInput* in = external(id)) return &in->result;
    const auto at = tree_.index_of(id);
    if (!at) {
      return error("feature.reference.broken",
                   fmt::format("the {} it uses no longer exists", what));
    }
    const SubjectRef other = subject_of(id);
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
      const SubjectRef other = subject_of(id);
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
        const SubjectRef other = subject_of(f->feature);
        return error("feature.reference.wrong_kind",
                     fmt::format("'{}' is not a plane or a sketch", other.label), {other});
      }
      return *(*r)->plane;
    }
    const auto& face = std::get<FaceRef>(ref);
    if (const ExternalInput* in = external(face.feature)) {
      // An input's face gives its plane directly; the body needn't touch it.
      const auto named = std::find_if(in->result.faces.begin(), in->result.faces.end(),
                                      [&](const NamedFace& n) { return n.name == face.name; });
      if (named == in->result.faces.end()) {
        return error("feature.reference.broken",
                     fmt::format("the face '{}' of '{}' no longer exists", face.name,
                                 in->subject.label),
                     {in->subject});
      }
      if (named->cylinder) {
        return error("feature.reference.wrong_kind",
                     fmt::format("the face '{}' of '{}' is curved; it needs a flat face",
                                 face.name, in->subject.label),
                     {in->subject});
      }
      return frame_for(named->plane);
    }
    auto found = faces_of(face);
    if (!found) return found.error();
    if (found->named->cylinder) {
      const SubjectRef other = subject_of(face.feature);
      return error("feature.reference.wrong_kind",
                   fmt::format("the face '{}' of '{}' is curved; it needs a flat face",
                               face.name, other.label),
                   {other});
    }
    return frame_for(found->named->plane);
  }

  struct FoundFaces {
    const NamedFace* named = nullptr;
    std::vector<Face> faces;  // the body's faces on it, at least one
  };

  // The body's faces (as this feature sees it) that a FaceRef names.
  Result<FoundFaces> faces_of(const FaceRef& face) {
    auto r = use(face.feature, "face's feature");
    if (!r) return r.error();
    const SubjectRef other = subject_of(face.feature);
    const auto named = std::find_if((*r)->faces.begin(), (*r)->faces.end(),
                                    [&](const NamedFace& n) { return n.name == face.name; });
    if (named == (*r)->faces.end()) {
      std::string why = fmt::format("the face '{}' of '{}' no longer exists", face.name,
                                    other.label);
      if (face.name.starts_with("side:")) {
        why += "; the sketch line (or arc or circle) that made it was probably deleted or is no "
               "longer part of a profile";
      }
      return error("feature.reference.broken", why, {other});
    }
    FoundFaces out;
    out.named = &*named;
    for (const Face& f : kernel::faces(body_)) {
      if (on_named(*named, f, settings_.linear_tolerance_mm)) out.faces.push_back(f);
    }
    if (out.faces.empty()) {
      return error("feature.reference.broken",
                   fmt::format("the face '{}' of '{}' is no longer on the body; a later feature "
                               "removed it",
                               face.name, other.label),
                   {other});
    }
    return out;
  }

  // The body's edges an EdgeRef names (seams excluded), each once.
  Result<std::vector<kernel::Edge>> edges_of(const EdgeRef& ref) {
    auto a = faces_of(ref.face);
    if (!a) return a.error();
    std::vector<kernel::Edge> out;
    if (!ref.other) {
      for (const Face& f : a->faces) {
        for (const auto& e : kernel::edges(f)) {
          if (!kernel::is_seam(e, f)) add_unique(out, e);
        }
      }
      return out;
    }
    auto b = faces_of(*ref.other);
    if (!b) return b.error();
    std::vector<kernel::Edge> b_edges;
    for (const Face& f : b->faces) {
      for (const auto& e : kernel::edges(f)) b_edges.push_back(e);
    }
    for (const Face& f : a->faces) {
      for (const auto& e : kernel::edges(f)) {
        const bool shared = std::any_of(b_edges.begin(), b_edges.end(),
                                        [&](const kernel::Edge& be) { return be.is_same(e); });
        if (shared) add_unique(out, e);
      }
    }
    if (out.empty()) {
      const SubjectRef fa = subject_of(ref.face.feature);
      const SubjectRef fb = subject_of(ref.other->feature);
      std::vector<SubjectRef> more{fa};
      if (!(ref.other->feature == ref.face.feature)) more.push_back(fb);
      return error("feature.reference.broken",
                   fmt::format("the face '{}' of '{}' and the face '{}' of '{}' no longer meet "
                               "in an edge",
                               ref.face.name, fa.label, ref.other->name, fb.label),
                   std::move(more));
    }
    return out;
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
        const SubjectRef other = subject_of(f->feature);
        return error("feature.reference.wrong_kind",
                     fmt::format("'{}' is not an axis", other.label), {other});
      }
      return *(*r)->axis;
    }
    const auto& e = std::get<SketchEntityRef>(ref);
    auto r = use_sketch(e.sketch);
    if (!r) return r.error();
    const sketch::Sketch& sk = *(*r)->sketch;
    const SubjectRef other = subject_of(e.sketch);
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
        const SubjectRef other = subject_of(f->feature);
        return error("feature.reference.wrong_kind",
                     fmt::format("'{}' is not a point", other.label), {other});
      }
      return *(*r)->point;
    }
    const auto& e = std::get<SketchEntityRef>(ref);
    auto r = use_sketch(e.sketch);
    if (!r) return r.error();
    const sketch::Sketch& sk = *(*r)->sketch;
    const SubjectRef other = subject_of(e.sketch);
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

  // Adds `tools` to the body or cuts them from it, in one boolean.
  Result<Solid> combine(std::vector<Solid> tools, Operation op) {
    if (tools.empty()) return body_;
    if (op == Operation::Add && body_.is_empty()) {
      const Solid first = tools.front();
      tools.erase(tools.begin());
      if (tools.empty()) return first;
      auto r = kernel::unite(first, tools, fuzzy());
      if (!r) return wrap(r.error());
      return r;
    }
    if (body_.is_empty()) {
      return error("feature.cut.no_body",
                   "there is no body to cut yet; add material with a feature above it first");
    }
    auto r = op == Operation::Add ? kernel::unite(body_, tools, fuzzy())
                                  : kernel::cut(body_, tools, fuzzy());
    if (!r) return wrap(r.error());
    return r;
  }

  // Unites the pieces of one feature's tool.
  Result<Solid> merge(const std::vector<Solid>& pieces) {
    if (pieces.size() == 1) return pieces.front();
    auto r = kernel::unite(pieces.front(), std::span<const Solid>(pieces).subspan(1), fuzzy());
    if (!r) return wrap(r.error());
    return r;
  }

  // Records what a solid feature added or cut, for patterns.
  void set_tool(const Solid& tool, Operation op) {
    result_.tool = tool;
    result_.operation = op;
  }

  // On a kernel failure, finds roughly the largest value below `failed`
  // for which `works` succeeds: halving up to three times, then bisecting.
  // At most six more kernel calls, and only after a failure.
  template <typename Works>
  std::optional<double> largest_working(double failed, Works&& works) {
    const double floor = settings_.min_feature_size_mm();
    double hi = failed;
    double lo = 0.0;
    for (int i = 0; i < 3 && lo == 0.0; ++i) {
      const double v = hi / 2;
      if (v < floor) break;
      if (works(v)) lo = v;
      else hi = v;
    }
    if (lo == 0.0) return std::nullopt;
    for (int i = 0; i < 3; ++i) {
      const double mid = (lo + hi) / 2;
      if (works(mid)) lo = mid;
      else hi = mid;
    }
    return lo;
  }

  // A kernel failure plus what the fallback search found: the value that
  // works, appended to the error and also as a "feature.fallback.works"
  // info diagnostic (so tools and tests can find it by code), or advice
  // when nothing smaller works either.
  Diagnostics with_fallback(Diagnostic failure, std::optional<double> works,
                            std::string_view what, std::string_view otherwise) {
    Diagnostics ds;
    if (works) {
      const std::string hint = fmt::format("a {} of about {:.3g} mm works here", what, *works);
      failure.message += fmt::format(" ({})", hint);
      ds.add(std::move(failure));
      ds.add(Diagnostic::info("feature.fallback.works",
                              fmt::format("'{}': {}", self_.label, hint), {self_}));
    } else {
      failure.message += fmt::format(" ({})", otherwise);
      ds.add(std::move(failure));
    }
    return ds;
  }

  struct Profiles {
    std::vector<Face> faces;
    std::vector<Id> lines;   // the line entities along the profiles' loops
    std::vector<Id> curves;  // the circle and arc entities along them
    std::vector<std::size_t> holes;  // the number of holes in each profile
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
      out.holes.push_back(p.holes.size());
      auto add_curves = [&](const sketch::ProfileLoop& loop) {
        for (const auto& e : loop.edges) {
          const auto* entity = sk.find_entity(e.entity);
          if (!entity) continue;
          if (entity->kind() == sketch::EntityKind::Line) out.lines.push_back(e.entity);
          else if (entity->kind() != sketch::EntityKind::Point) out.curves.push_back(e.entity);
        }
      };
      add_curves(p.outer);
      for (const auto& h : p.holes) add_curves(h);
    }
    return out;
  }

  // Names the side faces of `tool` made by the sketch's lines and, when
  // `curved` (no draft), its circles and arcs.
  void name_sides(const Solid& tool, const sketch::Sketch& sk, const Profiles& profiles,
                  const dvec3& dir, Operation op, bool curved) {
    const auto tool_faces = kernel::faces(tool);
    std::vector<std::optional<Plane>> planes;
    planes.reserve(tool_faces.size());
    for (const Face& f : tool_faces) planes.push_back(kernel::face_plane(f));
    const double tol = settings_.linear_tolerance_mm;
    if (curved) {
      for (const Id& id : profiles.curves) {
        dvec2 centre{};
        double radius = 0.0;
        if (const auto c = sketch::circle_geometry(sk, id)) {
          centre = c->center;
          radius = c->radius;
        } else if (const auto a = sketch::arc_geometry(sk, id)) {
          centre = a->center;
          radius = a->radius;
        } else {
          continue;
        }
        const Axis axis{sk.plane().to_model(centre), dir};
        // The tool's own face says which way it faces (a concave arc of an
        // outline faces its axis just like a hole does).
        for (const Face& f : tool_faces) {
          const auto c = kernel::face_cylinder(f);
          if (!c) continue;
          NamedFace probe{{}, {}, kernel::Cylinder{axis, radius, c->inward}};
          if (!on_named(probe, f, tol)) continue;
          kernel::Cylinder cyl{axis, radius, op == Operation::Add ? c->inward : !c->inward};
          result_.faces.push_back({"side:" + id.to_string(), Plane{}, cyl});
          break;
        }
      }
    }
    for (const Id& id : profiles.lines) {
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
        result_.faces.push_back(
            {"side:" + id.to_string(), Plane{(a + b) * 0.5, normal}, std::nullopt});
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
    const SubjectRef sketch_subject = subject_of(ex.sketch);
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
    set_tool(tool, ex.operation);

    const dvec3 start_point = plane.origin + dir * start;
    const dvec3 end_point = start_point + dir * depth;
    if (ex.operation == Operation::Add) {
      result_.faces.push_back({"start", Plane{start_point, -dir}, std::nullopt});
      result_.faces.push_back({"end", Plane{end_point, dir}, std::nullopt});
    } else if (ex.extent == Extent::Blind || ex.extent == Extent::UpToPlane) {
      result_.faces.push_back({"end", Plane{end_point, -dir}, std::nullopt});
    }
    name_sides(tool, sk, *profiles, dir, ex.operation, /*curved=*/*draft == 0.0);
    return {};
  }

  Diagnostics build(const RevolveFeature& rv) {
    auto sr = use_sketch(rv.sketch);
    if (!sr) return one(sr.error());
    const sketch::Sketch& sk = *(*sr)->sketch;
    const SubjectRef sketch_subject = subject_of(rv.sketch);
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
    set_tool(tool, rv.operation);

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
      result_.faces.push_back({"start", Plane{c, start_normal * s}, std::nullopt});
      result_.faces.push_back({"end", Plane{r.apply_point(c), end_normal * s}, std::nullopt});
    }
    return {};
  }

  // --- sweep and loft --------------------------------------------------------

  // The planar face of `tool` that contains `point` and satisfies `facing`
  // (given its outward normal), named for the body: its normal is flipped
  // for a cut, since a cut's tool faces into the material.
  template <typename Facing>
  void name_cap(const Solid& tool, std::string name, const dvec3& point, Operation op,
                Facing&& facing) {
    const double tol = settings_.linear_tolerance_mm;
    std::optional<Plane> best;
    double best_score = 0.0;
    for (const Face& f : kernel::faces(tool)) {
      const auto fp = kernel::face_plane(f);
      if (!fp || std::abs(dot(point - fp->origin, fp->normal)) > tol) continue;
      const double score = facing(fp->normal);
      if (score > best_score) {
        best_score = score;
        best = fp;
      }
    }
    if (!best) return;
    const dvec3 normal = op == Operation::Add ? best->normal : -best->normal;
    result_.faces.push_back({std::move(name), Plane{point, normal}, std::nullopt});
  }

  // The path drawn in a sketch, as one chain of model-space segments. An
  // open chain starts at the end nearer `near`; a closed one at the joint
  // nearest it.
  Result<kernel::Path> path_of(const sketch::Sketch& sk, const SubjectRef& sketch_subject,
                               const dvec3& near) {
    const SketchPlane& plane = sk.plane();
    struct Curve {
      Id id;
      kernel::ProfileSegment segment;  // forwards
      std::size_t ends[2] = {0, 0};    // joint indices of its start and end
    };
    std::vector<Curve> curves;
    std::vector<Id> circles;
    for (const auto& e : sk.entities()) {
      if (e.construction) continue;
      if (e.kind() == sketch::EntityKind::Line) {
        const auto g = sketch::line_geometry(sk, e.id);
        if (!g) continue;
        curves.push_back({e.id, kernel::LineSegment{plane.to_model(g->start),
                                                    plane.to_model(g->end)},
                        {0, 0}});
      } else if (e.kind() == sketch::EntityKind::Arc) {
        const auto g = sketch::arc_geometry(sk, e.id);
        if (!g) continue;
        curves.push_back({e.id, kernel::ArcSegment{
                                    plane.to_model(g->start()),
                                    plane.to_model(g->at((g->start_angle + g->end_angle) / 2)),
                                    plane.to_model(g->end())},
                        {0, 0}});
      } else if (e.kind() == sketch::EntityKind::Circle) {
        circles.push_back(e.id);
      }
    }
    if (curves.empty() && circles.empty()) {
      return error("feature.sweep.no_path",
                   fmt::format("'{}' has no lines, arcs or circles to sweep along",
                               sketch_subject.label),
                   {sketch_subject});
    }
    if (!circles.empty()) {
      if (circles.size() > 1 || !curves.empty()) {
        return error("feature.sweep.path_disconnected",
                     fmt::format("the path in '{}' must be one chain of curves; a circle can "
                                 "only be a path on its own",
                                 sketch_subject.label),
                     {sketch_subject});
      }
      const auto g = sketch::circle_geometry(sk, circles.front());
      if (!g) return error("feature.sweep.no_path", "the path's circle has no geometry");
      // Start at the circle's point nearest `near`, as two half arcs.
      dvec2 u{1, 0};
      const dvec3 c = plane.to_model(g->center);
      const dvec3 toward = near - c;
      const dvec2 local{dot(toward, plane.x_axis), dot(toward, plane.y_axis)};
      if (core::length(local) > 1e-12) u = local / core::length(local);
      const dvec2 v{-u.y, u.x};
      const double r = g->radius;
      auto at = [&](dvec2 d) { return plane.to_model(g->center + d * r); };
      kernel::Path path;
      path.segments.emplace_back(kernel::ArcSegment{at(u), at(v), at(-u)});
      path.segments.emplace_back(kernel::ArcSegment{at(-u), at(-v), at(u)});
      return path;
    }

    // Join curve ends into joints.
    const double tol = settings_.linear_tolerance_mm;
    std::vector<dvec3> joints;
    std::vector<int> degree;
    auto joint_of = [&](const dvec3& p) {
      for (std::size_t j = 0; j < joints.size(); ++j) {
        if (core::distance(joints[j], p) <= tol) {
          ++degree[j];
          return j;
        }
      }
      joints.push_back(p);
      degree.push_back(1);
      return joints.size() - 1;
    };
    for (auto& c : curves) {
      c.ends[0] = joint_of(seg_start(c.segment));
      c.ends[1] = joint_of(seg_end(c.segment));
    }
    for (std::size_t j = 0; j < joints.size(); ++j) {
      if (degree[j] > 2) {
        return error("feature.sweep.path_branches",
                     fmt::format("the path in '{}' branches: {} curves meet at one point; a "
                                 "path must be a single chain",
                                 sketch_subject.label, degree[j]),
                     {sketch_subject});
      }
    }
    // Start at a free end (open) or any joint (closed), nearest `near`.
    std::optional<std::size_t> start;
    bool open = false;
    for (std::size_t j = 0; j < joints.size(); ++j) {
      if (degree[j] == 1) open = true;
    }
    for (std::size_t j = 0; j < joints.size(); ++j) {
      if (open && degree[j] != 1) continue;
      if (!start || core::distance(joints[j], near) < core::distance(joints[*start], near)) {
        start = j;
      }
    }
    kernel::Path path;
    std::vector<bool> used(curves.size(), false);
    std::size_t at = *start;
    for (;;) {
      bool moved = false;
      for (std::size_t i = 0; i < curves.size(); ++i) {
        if (used[i]) continue;
        if (curves[i].ends[0] == at) {
          path.segments.push_back(curves[i].segment);
          at = curves[i].ends[1];
        } else if (curves[i].ends[1] == at) {
          path.segments.push_back(reversed(curves[i].segment));
          at = curves[i].ends[0];
        } else {
          continue;
        }
        used[i] = true;
        moved = true;
        break;
      }
      if (!moved) break;
    }
    if (path.segments.size() != curves.size()) {
      return error("feature.sweep.path_disconnected",
                   fmt::format("the path in '{}' is in separate pieces; join its curves end to "
                               "end into one chain",
                               sketch_subject.label),
                   {sketch_subject});
    }
    return path;
  }

  Diagnostics build(const SweepFeature& sw) {
    auto pr = use_sketch(sw.profile);
    if (!pr) return one(pr.error());
    auto pa = use_sketch(sw.path);
    if (!pa) return one(pa.error());
    const sketch::Sketch& profile_sketch = *(*pr)->sketch;
    const SubjectRef profile_subject = subject_of(sw.profile);
    auto profiles = profiles_of(profile_sketch, profile_subject);
    if (!profiles) return one(profiles.error());
    const auto centre = kernel::face_plane(profiles->faces.front());
    const dvec3 near = centre ? centre->origin : profile_sketch.plane().origin;
    auto path = path_of(*(*pa)->sketch, subject_of(sw.path), near);
    if (!path) return one(path.error());

    const auto orientation = sw.orientation == SweepOrientation::Fixed
                                 ? kernel::SweepOrientation::Fixed
                                 : kernel::SweepOrientation::FollowPath;
    std::vector<Solid> pieces;
    for (const Face& face : profiles->faces) {
      auto piece = kernel::make_sweep(face, *path, orientation, settings_);
      if (!piece) return one(wrap(piece.error()));
      pieces.push_back(*piece);
    }
    auto tool = merge(pieces);
    if (!tool) return one(tool.error());
    auto body = combine(*tool, sw.operation);
    if (!body) return one(body.error());
    set_body(*body);
    set_tool(*tool, sw.operation);

    // Caps of an open path.
    const auto& segs = path->segments;
    const dvec3 p0 = seg_start(segs.front());
    const dvec3 p1 = seg_end(segs.back());
    if (core::distance(p0, p1) > settings_.linear_tolerance_mm) {
      const dvec3 t0 = tangent(segs.front(), false);
      const dvec3 t1 = tangent(segs.back(), true);
      const dvec3 n = profile_sketch.plane().normal();
      name_cap(*tool, "start", near, sw.operation,
               [&](const dvec3& normal) { return parallel(normal, n) ? -dot(normal, t0) : 0.0; });
      if (std::abs(dot(p0 - profile_sketch.plane().origin, n)) <= settings_.linear_tolerance_mm) {
        name_cap(*tool, "end", p1, sw.operation,
                 [&](const dvec3& normal) { return dot(normal, t1); });
      }
    }
    return {};
  }

  Diagnostics build(const LoftFeature& lf) {
    if (lf.sections.size() < 2) {
      return one(error("feature.loft.too_few_sections",
                       fmt::format("a loft needs at least two section sketches; it has {}",
                                   lf.sections.size())));
    }
    std::vector<Face> sections;
    for (const Id& id : lf.sections) {
      auto sr = use_sketch(id);
      if (!sr) return one(sr.error());
      const SubjectRef subject = subject_of(id);
      auto profiles = profiles_of(*(*sr)->sketch, subject);
      if (!profiles) return one(profiles.error());
      if (profiles->faces.size() != 1 || profiles->holes.front() != 0) {
        return one(error("feature.loft.section_not_single",
                         fmt::format("each loft section must be one closed profile without "
                                     "holes, but '{}' has {} profile(s){}",
                                     subject.label, profiles->faces.size(),
                                     profiles->holes.front() != 0 ? " with holes" : ""),
                         {subject}));
      }
      sections.push_back(profiles->faces.front());
    }
    auto tool = kernel::make_loft(sections, lf.ruled, settings_);
    if (!tool) return one(wrap(tool.error()));
    auto body = combine(*tool, lf.operation);
    if (!body) return one(body.error());
    set_body(*body);
    set_tool(*tool, lf.operation);

    // The first and last sections' faces, facing away from the loft.
    const auto first = kernel::face_plane(sections.front());
    const auto last = kernel::face_plane(sections.back());
    if (first && last) {
      const dvec3 along = last->origin - first->origin;
      name_cap(*tool, "start", first->origin, lf.operation, [&](const dvec3& normal) {
        return parallel(normal, first->normal) ? -dot(normal, along) : 0.0;
      });
      name_cap(*tool, "end", last->origin, lf.operation, [&](const dvec3& normal) {
        return parallel(normal, last->normal) ? dot(normal, along) : 0.0;
      });
    }
    return {};
  }

  // --- shell, fillet and chamfer -----------------------------------------------

  Diagnostics build(const ShellFeature& sh) {
    if (sh.open.empty()) {
      return one(error("feature.shell.no_faces",
                       "it has no faces to open; choose at least one face to remove"));
    }
    auto thickness = eval(sh.thickness, "wall thickness");
    if (!thickness) return one(thickness.error());
    if (*thickness < settings_.min_feature_size_mm()) {
      return one(error("feature.shell.bad_thickness",
                       fmt::format("the wall thickness ({} mm) must be at least the minimum "
                                   "feature size ({} mm)",
                                   *thickness, settings_.min_feature_size_mm())));
    }
    std::vector<Face> open;
    for (const FaceRef& ref : sh.open) {
      auto found = faces_of(ref);
      if (!found) return one(found.error());
      for (const Face& f : found->faces) {
        const bool seen = std::any_of(open.begin(), open.end(),
                                      [&](const Face& o) { return o.is_same(f); });
        if (!seen) open.push_back(f);
      }
    }
    auto shelled = kernel::shell(body_, open, *thickness, sh.outward, settings_);
    if (!shelled) {
      Diagnostic d = wrap(shelled.error());
      if (d.code != "kernel.shell.failed") return one(std::move(d));
      const auto works = largest_working(*thickness, [&](double t) {
        return kernel::shell(body_, open, t, sh.outward, settings_).ok();
      });
      return with_fallback(std::move(d), works, "wall thickness",
                           "thinner walls fail too, so try opening other faces");
    }
    set_body(*shelled);
    return {};
  }

  // Fillets (`fillet`) or chamfers the edges the references name.
  Diagnostics round_edges(const std::vector<EdgeRef>& refs, const Param& size, bool fillet) {
    const std::string kind = fillet ? "feature.fillet" : "feature.chamfer";
    const std::string what = fillet ? "radius" : "distance";
    if (refs.empty()) {
      return one(error(kind + ".no_edges",
                       fmt::format("it has no edges to {}", fillet ? "round" : "bevel")));
    }
    auto value = eval(size, what);
    if (!value) return one(value.error());
    if (*value < settings_.min_feature_size_mm()) {
      return one(error(kind + (fillet ? ".bad_radius" : ".bad_distance"),
                       fmt::format("the {} ({} mm) must be at least the minimum feature size "
                                   "({} mm)",
                                   what, *value, settings_.min_feature_size_mm())));
    }
    std::vector<kernel::Edge> edges;
    for (const EdgeRef& ref : refs) {
      auto found = edges_of(ref);
      if (!found) return one(found.error());
      for (const auto& e : *found) add_unique(edges, e);
    }
    auto apply = [&](double v) {
      return fillet ? kernel::fillet(body_, edges, v) : kernel::chamfer(body_, edges, v);
    };
    auto result = apply(*value);
    if (!result) {
      Diagnostic d = wrap(result.error());
      if (d.code != (fillet ? "kernel.fillet.failed" : "kernel.chamfer.failed")) {
        return one(std::move(d));
      }
      const auto works = largest_working(*value, [&](double v) { return apply(v).ok(); });
      return with_fallback(std::move(d), works, what,
                           "smaller values fail too, so try fewer edges at a time");
    }
    set_body(*result);
    return {};
  }

  Diagnostics build(const FilletFeature& f) { return round_edges(f.edges, f.radius, true); }
  Diagnostics build(const ChamferFeature& c) { return round_edges(c.edges, c.distance, false); }

  // --- patterns -------------------------------------------------------------

  Result<long long> count_of(const Param& p) {
    auto c = eval(p, "count");
    if (!c) return c.error();
    const double rounded = std::round(*c);
    if (std::abs(*c - rounded) > 1e-9 || rounded < 1) {
      return error("feature.pattern.bad_count",
                   fmt::format("the count ({:.6g}) must be a whole number, at least 1", *c));
    }
    if (rounded > static_cast<double>(kMaxPatternCount)) {
      return error("feature.pattern.too_many",
                   fmt::format("the count ({:.0f}) is above the limit of {}", rounded,
                               kMaxPatternCount));
    }
    return static_cast<long long>(rounded);
  }

  // Applies copies of `features` placed by `placements` (one transform per
  // copy, the original excluded).
  Diagnostics pattern(const std::vector<Id>& features, const std::vector<dtransform>& placements) {
    if (features.empty()) {
      return one(error("feature.pattern.no_features", "it has no features to copy"));
    }
    std::vector<const FeatureResult*> seeds;
    for (const Id& id : features) {
      auto r = use(id, "feature to copy");
      if (!r) return one(r.error());
      if (!(*r)->operation || (*r)->tool.is_empty()) {
        const SubjectRef other = subject_of(id);
        return one(error("feature.pattern.not_solid_feature",
                         fmt::format("'{}' doesn't add or cut material, so it can't be copied; "
                                     "pattern extrudes, revolves, sweeps and lofts",
                                     other.label),
                         {other}));
      }
      seeds.push_back(*r);
    }
    // combine() works on body_; step it along seed by seed and put it back
    // afterwards, since body_ is what a failure falls back to.
    const Solid original = body_;
    Diagnostics problems;
    for (const FeatureResult* seed : seeds) {
      std::vector<Solid> copies;
      copies.reserve(placements.size());
      for (std::size_t k = 0; k < placements.size(); ++k) {
        auto copy = kernel::transformed(seed->tool, placements[k]);
        if (!copy) {
          problems.add(wrap(copy.error()));
          break;
        }
        copies.push_back(*copy);
        for (const NamedFace& f : seed->faces) {
          result_.faces.push_back(transformed(
              f, placements[k],
              fmt::format("{}:{}:{}", k + 1, seed->feature.to_string(), f.name)));
        }
      }
      if (problems.has_errors()) break;
      auto combined = combine(std::move(copies), *seed->operation);
      if (!combined) {
        problems.add(combined.error());
        break;
      }
      body_ = *combined;
    }
    const Solid patterned = body_;
    body_ = original;
    if (problems.has_errors()) return problems;
    set_body(patterned);
    return {};
  }

  Diagnostics build(const LinearPatternFeature& lp) {
    auto count = count_of(lp.count);
    if (!count) return one(count.error());
    auto axis = axis_of(lp.direction);
    if (!axis) return one(axis.error());
    auto spacing = eval(lp.spacing, "spacing");
    if (!spacing) return one(spacing.error());
    if (*spacing < settings_.min_feature_size_mm()) {
      return one(error("feature.pattern.bad_spacing",
                       fmt::format("the spacing ({} mm) must be at least the minimum feature "
                                   "size ({} mm); use 'reverse' to go the other way",
                                   *spacing, settings_.min_feature_size_mm())));
    }
    const dvec3 step = normalize(axis->direction) * (*spacing * (lp.reverse ? -1.0 : 1.0));
    std::vector<dtransform> placements;
    for (long long k = 1; k < *count; ++k) {
      placements.push_back(dtransform::translate(step * static_cast<double>(k)));
    }
    return pattern(lp.features, placements);
  }

  Diagnostics build(const CircularPatternFeature& cp) {
    auto count = count_of(cp.count);
    if (!count) return one(count.error());
    auto axis = axis_of(cp.axis);
    if (!axis) return one(axis.error());
    auto angle = eval(cp.angle, "angle");
    if (!angle) return one(angle.error());
    if (*angle <= 0.0 || *angle > kTwoPi + 1e-9) {
      return one(error("feature.pattern.bad_angle",
                       fmt::format("the angle ({:.6g}°) must be above 0 and at most 360°",
                                   *angle * 180.0 / std::numbers::pi)));
    }
    const bool full = *angle >= kTwoPi - 1e-9;
    const double n = static_cast<double>(*count);
    const double step = full ? kTwoPi / n : (*count > 1 ? *angle / (n - 1) : 0.0);
    const dvec3 d = normalize(axis->direction) * (cp.reverse ? -1.0 : 1.0);
    std::vector<dtransform> placements;
    for (long long k = 1; k < *count; ++k) {
      placements.push_back(dtransform::translate(axis->origin) *
                           dtransform::rotate(d, step * static_cast<double>(k)) *
                           dtransform::translate(-axis->origin));
    }
    return pattern(cp.features, placements);
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

const ExternalInput* ExternalInputs::find(const Id& id) const {
  for (const auto& in : references) {
    if (in.subject.id == id) return &in;
  }
  return nullptr;
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
  if (options.inputs) {
    // Variables of the tree win over input values of the same name, failed
    // ones too: an input never stands in for a variable that has no value.
    for (const auto& [name, value] : options.inputs->values) {
      if (tree.variables().find(name) != nullptr) continue;
      out.variables.values.try_emplace(name, value);
    }
  }
  const auto& features = tree.features();
  out.features.reserve(features.size());

  std::size_t first = 0;
  const bool has_inputs = options.inputs && !options.inputs->references.empty();
  if (options.previous && !options.start.is_null() && !has_inputs) {
    const RebuildResult& prev = *options.previous;
    const auto k = tree.index_of(options.start);
    bool reusable = k.has_value() && prev.variables == out.variables &&
                    prev.settings == settings && prev.features.size() >= *k &&
                    prev.built_from.size() >= *k;
    for (std::size_t i = 0; reusable && i < *k; ++i) {
      reusable = prev.features[i].feature == features[i].id &&
                 prev.built_from[i] == features[i] &&
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
    Builder builder(tree, settings, out, i, options.inputs);
    out.features.push_back(builder.run());
  }
  for (const auto& r : out.features) out.diagnostics.append(r.diagnostics);
  if (!out.features.empty()) out.body = out.features.back().body;
  out.built_from = features;
  out.settings = settings;
  return out;
}

}  // namespace vs::feature

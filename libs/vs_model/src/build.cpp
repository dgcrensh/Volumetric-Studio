#include "vs/model/build.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>

#include <fmt/format.h>

#include "vs/feature/expression.hpp"
#include "vs/kernel/check.hpp"
#include "vs/kernel/exchange.hpp"
#include "vs/kernel/operations.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/model/hierarchy.hpp"
#include "vs/sketch/profile.hpp"

namespace vs::model {

using core::dvec2;
using core::SubjectKind;
using feature::FeatureResult;
using feature::NamedFace;
using kernel::Solid;
using sketch::SketchPlane;

std::string_view to_string(VolumeStatus status) {
  switch (status) {
    case VolumeStatus::Current: return "current";
    case VolumeStatus::Stale: return "stale";
    case VolumeStatus::Regenerating: return "regenerating";
    case VolumeStatus::Failed: return "failed";
  }
  return "stale";
}

bool CylinderBuild::band() const { return sweep >= 2.0 * std::numbers::pi; }

SketchPlane cylinder_frame(const kernel::Axis& axis) {
  const core::dvec3 n = core::normalize(axis.direction);
  // The principal axis least aligned with the direction (the first such,
  // in x, y, z order), made perpendicular to it.
  const std::array<core::dvec3, 3> principal{core::dvec3{1, 0, 0}, core::dvec3{0, 1, 0},
                                             core::dvec3{0, 0, 1}};
  std::size_t best = 0;
  for (std::size_t i = 1; i < 3; ++i) {
    if (std::abs(core::dot(principal[i], n)) < std::abs(core::dot(principal[best], n))) best = i;
  }
  const core::dvec3 x = core::normalize(principal[best] - n * core::dot(principal[best], n));
  return SketchPlane{axis.origin, x, core::cross(n, x)};
}

const VolumeBuild* DocumentBuild::find_volume(const Id& volume) const {
  for (const auto& v : volumes) {
    if (v.volume == volume) return &v;
  }
  return nullptr;
}

const ConnectionBuild* DocumentBuild::find_connection(const Id& surface) const {
  for (const auto& c : connections) {
    if (c.surface == surface) return &c;
  }
  return nullptr;
}

namespace {

Diagnostic about(const SubjectRef& subject, Diagnostic d) {
  d.message = fmt::format("'{}': {}", subject.label, d.message);
  d.subjects.insert(d.subjects.begin(), subject);
  return d;
}

Diagnostic error(const SubjectRef& subject, std::string code, std::string message,
                 std::vector<SubjectRef> more = {}) {
  std::vector<SubjectRef> subjects{subject};
  subjects.insert(subjects.end(), more.begin(), more.end());
  return Diagnostic::error(std::move(code), fmt::format("'{}': {}", subject.label, message),
                           std::move(subjects));
}

class DocumentBuilder {
 public:
  // Builds the layout into `out`.
  DocumentBuilder(const Document& doc, const ProductSettings& settings, DocumentBuild& out)
      : doc_(doc), settings_(settings), blobs_(nullptr), in_(out), out_(&out) {}
  // Builds contents from a finished layout, which it only reads.
  DocumentBuilder(const Document& doc, const ProductSettings& settings, const BlobStore* blobs,
                  const DocumentBuild& layout)
      : doc_(doc), settings_(settings), blobs_(blobs), in_(layout), out_(nullptr) {}

  // The layout, boundaries, connection surfaces and parameters, level by
  // level (L3): the root's, then each composite's, parents first.
  void run_layout() {
    out_->layout = feature::rebuild(doc_.layout, settings_);

    raw_.resize(doc_.volumes.size());
    out_->volumes.resize(doc_.volumes.size());
    for (std::size_t i = 0; i < doc_.volumes.size(); ++i) {
      out_->volumes[i].volume = doc_.volumes[i].id;
      // Volumes no level reaches (a missing or non-composite parent:
      // check() reports it) stay failed, with no boundary.
      out_->volumes[i].status = VolumeStatus::Failed;
    }
    out_->connections.resize(doc_.connections.size());
    for (std::size_t k = 0; k < doc_.connections.size(); ++k) {
      out_->connections[k].surface = doc_.connections[k].id;
    }
    build_level({});
  }

  // The layout's diagnostics, then each volume's, then each surface's.
  void collect_diagnostics() {
    out_->diagnostics = out_->layout.diagnostics;
    for (const auto& v : out_->volumes) out_->diagnostics.append(v.diagnostics);
    for (const auto& c : out_->connections) out_->diagnostics.append(c.diagnostics);
  }

 private:
  const Document& doc_;
  const ProductSettings& settings_;
  const BlobStore* blobs_;
  const DocumentBuild& in_;  // what the builder reads
  DocumentBuild* out_;       // what it writes: null when building contents
  std::vector<Solid> raw_;  // boundaries before keep-outs
  // The level being built (L3): null for the root layout, else a composite
  // volume, whose layout rebuild and inputs the lookups below use.
  Id level_;
  std::map<Id, feature::ExternalInputs> level_inputs_;

  kernel::BooleanOptions fuzzy() const { return {settings_.linear_tolerance_mm}; }

  std::size_t index_of(const Id& volume) const {
    for (std::size_t i = 0; i < doc_.volumes.size(); ++i) {
      if (doc_.volumes[i].id == volume) return i;
    }
    return doc_.volumes.size();
  }

  // --- levels (L3) ------------------------------------------------------------------

  void build_level(const Id& level) {
    level_ = level;
    const std::vector<std::size_t> kids = children(doc_, level);
    for (const std::size_t i : kids) {
      raw_[i] = raw_boundary(doc_.volumes[i], out_->volumes[i].diagnostics);
    }
    for (const std::size_t i : kids) {
      cut_keep_outs(i);
      VolumeBuild& vb = out_->volumes[i];
      if (!vb.boundary.is_empty()) vb.boundary_faces = boundary_faces(doc_.volumes[i], vb.boundary);
    }
    for (std::size_t k = 0; k < doc_.connections.size(); ++k) {
      const ConnectionSurface& c = doc_.connections[k];
      if (!c.volumes.empty() && doc_.find_volume(c.volumes.front()) != nullptr &&
          level_of(doc_, c) == level) {
        out_->connections[k] = connection(c);
      }
    }
    for (const std::size_t i : kids) parameters(i);

    for (const std::size_t i : kids) {
      if (!doc_.volumes[i].composite()) continue;
      composite_layout(i);
      build_level(doc_.volumes[i].id);
      level_ = level;
    }
  }

  // A composite's layout, rebuilt with its inputs (as a parametric tree is);
  // its children are built from it.
  void composite_layout(std::size_t i) {
    const Volume& v = doc_.volumes[i];
    VolumeBuild& vb = out_->volumes[i];
    if (vb.boundary.is_empty()) return;  // failed already; its children fail with it
    feature::ExternalInputs& inputs = level_inputs_[v.id];
    inputs = inputs_for(v, vb);
    feature::RebuildOptions options;
    options.inputs = &inputs;
    vb.contents_rebuild =
        feature::rebuild(std::get<CompositeContents>(v.contents).layout, settings_, options);
    const auto& r = *vb.contents_rebuild;
    if (!r.ok()) {
      vb.diagnostics.add(error(doc_.volume_subject(v.id), "model.contents.failed",
                               "its layout didn't rebuild cleanly"));
      vb.status = VolumeStatus::Failed;
    }
    vb.diagnostics.append(r.diagnostics);
  }

  // What a volume's tree sees: its boundary's named faces, the connection
  // surfaces joining it (delegated ones included) and its parameters.
  feature::ExternalInputs inputs_for(const Volume& v, const VolumeBuild& layout) const {
    feature::ExternalInputs inputs;
    {
      feature::ExternalInput boundary;
      boundary.subject = doc_.boundary_subject(v.id);
      boundary.result.faces = layout.boundary_faces;
      inputs.references.push_back(std::move(boundary));
    }
    for (std::size_t k = 0; k < doc_.connections.size(); ++k) {
      const ConnectionSurface& c = doc_.connections[k];
      if (!joins(doc_, c, v.id) || !in_.connections[k].ok) continue;
      const ConnectionBuild& cb = in_.connections[k];
      feature::ExternalInput in;
      in.subject = doc_.connection_subject(c.id);
      if (cb.cylinder) {
        const kernel::Axis axis{cb.frame.origin, cb.frame.normal()};
        in.result.axis = axis;
        in.result.faces.push_back(
            {"patch", {cb.frame.origin, cb.frame.normal()},
             kernel::Cylinder{axis, cb.cylinder->radius, /*inward=*/false}});
      } else {
        in.result.plane = cb.frame;
        in.result.faces.push_back({"patch", {cb.frame.origin, cb.frame.normal()}, std::nullopt});
      }
      inputs.references.push_back(std::move(in));
    }
    for (const auto& [name, value] : layout.parameters) inputs.values.emplace(name, value);
    return inputs;
  }

  // The level's rebuilt layout, and its tree for subjects.
  const feature::RebuildResult* level_rebuild() const {
    if (level_.is_null()) return &in_.layout;
    const std::size_t i = index_of(level_);
    if (i >= in_.volumes.size() || !in_.volumes[i].contents_rebuild) return nullptr;
    return &*in_.volumes[i].contents_rebuild;
  }
  const feature::FeatureTree& level_tree() const {
    static const feature::FeatureTree kEmpty;
    const feature::FeatureTree* t = level_layout(doc_, level_);
    return t != nullptr ? *t : kEmpty;
  }

  // --- layout lookups ------------------------------------------------------------

  Result<double> eval(const Param& p, const SubjectRef& by, std::string_view what) const {
    if (p.expression.empty()) {
      if (!std::isfinite(p.value)) {
        return error(by, "feature.expression.not_finite",
                     fmt::format("the {} isn't a finite number", what));
      }
      return p.value;
    }
    // The level's variables; inside a composite, then its parameters.
    const feature::RebuildResult* r = level_rebuild();
    const feature::Expression::Lookup variables =
        r != nullptr ? r->variables.lookup() : feature::Expression::Lookup{};
    const VolumeBuild* composite =
        level_.is_null() ? nullptr : &in_.volumes[index_of(level_)];
    const auto lookup = [&](std::string_view name) -> std::optional<double> {
      if (variables) {
        if (auto value = variables(name)) return value;
      }
      if (composite != nullptr) {
        const auto it = composite->parameters.find(name);
        if (it != composite->parameters.end()) return it->second;
      }
      return std::nullopt;
    };
    auto v = feature::evaluate_expression(p.expression, lookup);
    if (!v) {
      Diagnostic d = v.error();
      d.message = fmt::format("the {}: {}", what, d.message);
      return about(by, std::move(d));
    }
    return *v;
  }

  // A feature of the level's layout, if it built. Inside a composite, the
  // composite's inputs (its boundary's faces, the surfaces joining it) can
  // be used as they can by its own features.
  Result<const FeatureResult*> layout_result(const Id& id, const SubjectRef& by,
                                             std::string_view code) const {
    const feature::RebuildResult* level = level_rebuild();
    const FeatureResult* r = level != nullptr ? level->find(id) : nullptr;
    if (r == nullptr && !level_.is_null()) {
      const auto it = level_inputs_.find(level_);
      if (it != level_inputs_.end()) {
        if (const feature::ExternalInput* in = it->second.find(id)) return &in->result;
      }
    }
    const SubjectRef other = level_tree().subject(id);
    if (!r) {
      return error(by, std::string(code), "a layout feature it uses no longer exists");
    }
    if (!r->ok()) {
      return error(by, std::string(code),
                   fmt::format("it uses '{}', which {}", other.label,
                               r->status == feature::FeatureStatus::Suppressed
                                   ? "is suppressed"
                                   : "failed to rebuild"),
                   {other});
    }
    return r;
  }

  Result<dvec3> point_of(const feature::PointRef& ref, const SubjectRef& by) const {
    if (const auto* f = std::get_if<feature::FeatureRef>(&ref)) {
      auto r = layout_result(f->feature, by, "model.boundary.point_failed");
      if (!r) return r.error();
      if (!(*r)->point) {
        return error(by, "model.boundary.point_failed",
                     fmt::format("'{}' is not a point", level_tree().subject(f->feature).label));
      }
      return *(*r)->point;
    }
    const auto& e = std::get<feature::SketchEntityRef>(ref);
    auto r = layout_result(e.sketch, by, "model.boundary.point_failed");
    if (!r) return r.error();
    const auto* sk = (*r)->sketch.get();
    const auto p = sk ? sk->point(e.entity) : std::nullopt;
    if (!p) {
      return error(by, "model.boundary.point_failed",
                   fmt::format("the sketch point it uses isn't in '{}'",
                               level_tree().subject(e.sketch).label));
    }
    return sk->plane().to_model(*p);
  }

  Result<SketchPlane> plane_of(const feature::PlaneRef& ref, const SubjectRef& by) const {
    if (const auto* p = std::get_if<feature::PrincipalPlane>(&ref)) {
      return feature::principal_frame(*p);
    }
    const Id& id = std::holds_alternative<feature::FeatureRef>(ref)
                       ? std::get<feature::FeatureRef>(ref).feature
                       : std::get<feature::FaceRef>(ref).feature;
    auto r = layout_result(id, by, "model.connection.plane_failed");
    if (!r) return r.error();
    if (std::holds_alternative<feature::FeatureRef>(ref)) {
      if (!(*r)->plane) {
        return error(by, "model.connection.plane_failed",
                     fmt::format("'{}' is not a plane or a sketch", level_tree().subject(id).label));
      }
      return *(*r)->plane;
    }
    const auto& face = std::get<feature::FaceRef>(ref);
    for (const auto& n : (*r)->faces) {
      if (n.name == face.name && !n.cylinder) return feature::frame_for(n.plane);
    }
    return error(by, "model.connection.plane_failed",
                 fmt::format("'{}' has no flat face '{}'", level_tree().subject(id).label,
                             face.name));
  }

  Result<kernel::Axis> axis_of(const feature::AxisRef& ref, const SubjectRef& by) const {
    if (const auto* p = std::get_if<feature::PrincipalAxis>(&ref)) {
      switch (*p) {
        case feature::PrincipalAxis::X: return kernel::Axis{{0, 0, 0}, {1, 0, 0}};
        case feature::PrincipalAxis::Y: return kernel::Axis{{0, 0, 0}, {0, 1, 0}};
        case feature::PrincipalAxis::Z: return kernel::Axis{{0, 0, 0}, {0, 0, 1}};
      }
    }
    if (const auto* f = std::get_if<feature::FeatureRef>(&ref)) {
      auto r = layout_result(f->feature, by, "model.connection.axis_failed");
      if (!r) return r.error();
      if (!(*r)->axis) {
        return error(by, "model.connection.axis_failed",
                     fmt::format("'{}' is not an axis", level_tree().subject(f->feature).label));
      }
      return *(*r)->axis;
    }
    const auto& e = std::get<feature::SketchEntityRef>(ref);
    auto r = layout_result(e.sketch, by, "model.connection.axis_failed");
    if (!r) return r.error();
    const sketch::Sketch* sk = (*r)->sketch.get();
    const auto line = sk ? sketch::line_geometry(*sk, e.entity) : std::nullopt;
    if (!line) {
      return error(by, "model.connection.axis_failed",
                   fmt::format("the sketch line it uses as an axis isn't a line in '{}'",
                               level_tree().subject(e.sketch).label));
    }
    const dvec3 a = sk->plane().to_model(line->start);
    const dvec3 b = sk->plane().to_model(line->end);
    if (core::length(b - a) < settings_.min_feature_size_mm()) {
      return error(by, "model.connection.axis_failed", "its axis line is too short");
    }
    return kernel::Axis{a, core::normalize(b - a)};
  }

  // --- boundaries ------------------------------------------------------------------

  Solid raw_boundary(const Volume& v, Diagnostics& diags) const {
    const SubjectRef by = doc_.boundary_subject(v.id);
    if (const auto* fb = std::get_if<FeatureBoundary>(&v.boundary)) {
      std::vector<Solid> tools;
      for (const auto& id : fb->features) {
        auto r = layout_result(id, by, "model.boundary.feature_failed");
        if (!r) {
          diags.add(r.error());
          return {};
        }
        if ((*r)->tool.is_empty()) {
          diags.add(error(by, "model.boundary.feature_failed",
                          fmt::format("'{}' makes no solid", level_tree().subject(id).label),
                          {level_tree().subject(id)}));
          return {};
        }
        tools.push_back((*r)->tool);
      }
      if (tools.empty()) return {};
      if (tools.size() == 1) return tools.front();
      auto u = kernel::unite(tools.front(), std::span<const Solid>(tools).subspan(1), fuzzy());
      if (!u) {
        diags.add(about(by, u.error()));
        return {};
      }
      return *u;
    }
    const auto& box = std::get<BoxBoundary>(v.boundary);
    auto a = point_of(box.min, by);
    auto b = point_of(box.max, by);
    if (!a || !b) {
      diags.add(!a ? a.error() : b.error());
      return {};
    }
    const dvec3 lo{std::min(a->x, b->x), std::min(a->y, b->y), std::min(a->z, b->z)};
    const dvec3 hi{std::max(a->x, b->x), std::max(a->y, b->y), std::max(a->z, b->z)};
    const double min_size = settings_.min_feature_size_mm();
    if (hi.x - lo.x < min_size || hi.y - lo.y < min_size || hi.z - lo.z < min_size) {
      diags.add(error(by, "model.boundary.degenerate",
                      "its two corners don't span a box with volume"));
      return {};
    }
    auto s = kernel::make_box(core::dbox3::from_min_max(lo, hi), settings_);
    if (!s) {
      diags.add(about(by, s.error()));
      return {};
    }
    return *s;
  }

  void cut_keep_outs(std::size_t i) {
    const Volume& v = doc_.volumes[i];
    VolumeBuild& vb = out_->volumes[i];
    vb.boundary = raw_[i];
    if (vb.boundary.is_empty()) return;
    const SubjectRef by = doc_.boundary_subject(v.id);
    std::vector<Solid> cutters;
    for (const auto& k : doc_.keep_outs) {
      if (std::find(k.volumes.begin(), k.volumes.end(), v.id) == k.volumes.end()) continue;
      const SubjectRef ks = doc_.keep_out_subject(k.id);
      for (const auto& id : k.cutters) {
        if (const Volume* cutter = doc_.find_volume(id)) {
          // A keep-out volume whose boundary failed cuts nothing it should:
          // the target fails rather than keep the space it reserves.
          bool found = false;
          for (std::size_t j = 0; j < doc_.volumes.size(); ++j) {
            if (doc_.volumes[j].id == id && !raw_[j].is_empty()) {
              cutters.push_back(raw_[j]);
              found = true;
            }
          }
          if (!found) {
            vb.diagnostics.add(error(ks, "model.boundary.feature_failed",
                                     fmt::format("its cutter '{}' has no boundary",
                                                 doc_.volume_subject(cutter->id).label),
                                     {doc_.boundary_subject(cutter->id), by}));
            vb.boundary = Solid{};
            return;
          }
          continue;
        }
        auto r = layout_result(id, ks, "model.boundary.feature_failed");
        if (!r || (*r)->tool.is_empty()) {
          vb.diagnostics.add(r ? error(ks, "model.boundary.feature_failed",
                                       "a cutter makes no solid")
                               : r.error());
          vb.boundary = Solid{};
          return;
        }
        cutters.push_back((*r)->tool);
      }
    }
    if (cutters.empty()) return;
    auto cut = kernel::cut(vb.boundary, cutters, fuzzy());
    if (!cut && cut.error().code == "kernel.boolean.empty_result") {
      vb.diagnostics.add(error(by, "model.boundary.cut_away",
                               "its keep-out regions remove the whole boundary"));
      vb.boundary = Solid{};
      return;
    }
    if (!cut) {
      vb.diagnostics.add(about(by, cut.error()));
      vb.boundary = Solid{};
      return;
    }
    const auto mass = kernel::mass_properties(*cut);
    if (cut->is_empty() || !mass || mass->volume <= 0.0) {
      vb.diagnostics.add(error(by, "model.boundary.cut_away",
                               "its keep-out regions remove the whole boundary"));
      vb.boundary = Solid{};
      return;
    }
    vb.boundary = *cut;
  }

  // The boundary's named faces, for the contents' rebuild.
  std::vector<NamedFace> boundary_faces(const Volume& v, const Solid& boundary) const {
    std::vector<NamedFace> faces;
    if (const auto* fb = std::get_if<FeatureBoundary>(&v.boundary)) {
      for (const auto& id : fb->features) {
        const FeatureResult* r = level_rebuild() != nullptr ? level_rebuild()->find(id) : nullptr;
        if (!r) continue;
        for (const auto& n : r->faces) {
          NamedFace copy = n;
          copy.name = fmt::format("{}:{}", id.to_string(), n.name);
          faces.push_back(std::move(copy));
        }
      }
      return faces;
    }
    const auto raw_box = kernel::bounding_box(boundary);
    faces.push_back({"-x", {raw_box.min, {-1, 0, 0}}, std::nullopt});
    faces.push_back({"+x", {raw_box.max, {1, 0, 0}}, std::nullopt});
    faces.push_back({"-y", {raw_box.min, {0, -1, 0}}, std::nullopt});
    faces.push_back({"+y", {raw_box.max, {0, 1, 0}}, std::nullopt});
    faces.push_back({"-z", {raw_box.min, {0, 0, -1}}, std::nullopt});
    faces.push_back({"+z", {raw_box.max, {0, 0, 1}}, std::nullopt});
    return faces;
  }

  // --- connection surfaces -----------------------------------------------------------

  ConnectionBuild connection(const ConnectionSurface& c) const {
    ConnectionBuild cb;
    cb.surface = c.id;
    const SubjectRef by = doc_.connection_subject(c.id);
    auto fail = [&](Diagnostic d) {
      cb.diagnostics.add(std::move(d));
      cb.ok = false;
      return cb;
    };

    if (const auto* sp = std::get_if<SketchPatch>(&c.geometry)) {
      auto r = layout_result(sp->sketch, by, "model.connection.sketch_failed");
      if (!r) return fail(r.error());
      const sketch::Sketch* sk = (*r)->sketch.get();
      if (!sk) {
        return fail(error(by, "model.connection.sketch_failed",
                          fmt::format("'{}' is not a sketch", level_tree().subject(sp->sketch).label)));
      }
      auto profiles = sketch::extract_profiles(*sk, settings_);
      if (!profiles) return fail(about(by, profiles.error()));
      if (profiles->size() != 1) {
        return fail(error(by, "model.connection.profiles",
                          fmt::format("its sketch '{}' has {} closed profiles; a connection "
                                      "surface needs exactly one",
                                      level_tree().subject(sp->sketch).label, profiles->size()),
                          {level_tree().subject(sp->sketch)}));
      }
      auto face = sketch::make_face(*sk, profiles->front(), settings_);
      if (!face) return fail(about(by, face.error()));
      cb.patch = *face;
      cb.frame = sk->plane();
      cb.area = profiles->front().area();
    } else if (const auto* cp = std::get_if<CylinderPatch>(&c.geometry)) {
      auto axis = axis_of(cp->axis, by);
      if (!axis) return fail(axis.error());
      auto r = eval(cp->radius, by, "radius");
      auto a0 = eval(cp->start_angle, by, "start angle");
      auto sweep = eval(cp->sweep, by, "sweep");
      auto t0 = eval(cp->start, by, "start along the axis");
      auto len = eval(cp->length, by, "length");
      for (const auto* x : {&r, &a0, &sweep, &t0, &len}) {
        if (!*x) return fail(x->error());
      }
      constexpr double kTurn = 2.0 * std::numbers::pi;
      const double min = settings_.min_feature_size_mm();
      if (*r < min || *len < min || !(*sweep > 0.0) || *sweep * *r < min ||
          *sweep > kTurn + 1e-9) {
        return fail(error(by, "model.connection.bad_size",
                          fmt::format("its cylinder has radius {} mm, sweep {} rad and length {} "
                                      "mm; the radius, the arc and the length must be at least {} "
                                      "mm, and the sweep at most a full turn",
                                      *r, *sweep, *len, min)));
      }
      CylinderBuild cyl{*r, *a0, std::abs(*sweep - kTurn) <= 1e-9 ? kTurn : *sweep, *t0, *len};
      cb.frame = cylinder_frame(*axis);
      auto face = kernel::make_cylinder_face({cb.frame.origin, cb.frame.normal()}, cb.frame.x_axis,
                                             cyl.radius, cyl.start_angle, cyl.sweep, cyl.start,
                                             cyl.length, settings_);
      if (!face) return fail(about(by, face.error()));
      cb.patch = *face;
      cb.cylinder = cyl;
      cb.area = cyl.radius * cyl.sweep * cyl.length;
    } else {
      const auto& dp = std::get<DatumPatch>(c.geometry);
      auto frame = plane_of(dp.plane, by);
      if (!frame) return fail(frame.error());
      auto u = eval(dp.u, by, "patch centre u");
      auto v = eval(dp.v, by, "patch centre v");
      auto w = eval(dp.width, by, "patch width");
      auto h = eval(dp.height, by, "patch height");
      for (const auto* r : {&u, &v, &w, &h}) {
        if (!*r) return fail(r->error());
      }
      if (*w < settings_.min_feature_size_mm() || *h < settings_.min_feature_size_mm()) {
        return fail(error(by, "model.connection.bad_size",
                          fmt::format("its rectangle is {} x {} mm; both sides must be at least "
                                      "{} mm",
                                      *w, *h, settings_.min_feature_size_mm())));
      }
      const std::array<dvec2, 4> corners{dvec2{*u - *w / 2, *v - *h / 2},
                                         dvec2{*u + *w / 2, *v - *h / 2},
                                         dvec2{*u + *w / 2, *v + *h / 2},
                                         dvec2{*u - *w / 2, *v + *h / 2}};
      std::array<dvec3, 4> points{};
      for (std::size_t i = 0; i < 4; ++i) points[i] = frame->to_model(corners[i]);
      auto face = kernel::make_face(kernel::Loop::polygon(points), settings_);
      if (!face) return fail(about(by, face.error()));
      cb.patch = *face;
      cb.frame = *frame;
      cb.area = *w * *h;
    }

    if (const auto* cl = std::get_if<ClearanceParams>(&c.params)) {
      auto gap = eval(cl->gap, by, "gap");
      if (!gap) return fail(gap.error());
      cb.gap = *gap;
    }
    if (const auto* b = std::get_if<BoltedFlangeParams>(&c.params)) {
      if (cb.cylinder) {
        return fail(error(by, "model.connection.curved_flange",
                          "a bolted flange must be planar; use a sketch profile or a datum "
                          "rectangle, or make the surface bonded"));
      }
      auto wall = eval(b->min_wall, by, "minimum wall");
      if (!wall) return fail(wall.error());
      cb.min_wall = *wall;
      auto u = eval(b->holes.u, by, "hole pattern centre u");
      auto v = eval(b->holes.v, by, "hole pattern centre v");
      if (!u) return fail(u.error());
      if (!v) return fail(v.error());
      auto whole = [&](const Param& p, std::string_view what) -> Result<int> {
        auto n = eval(p, by, what);
        if (!n) return n.error();
        if (*n < 1 || *n > 1000 || std::abs(*n - std::round(*n)) > 1e-9) {
          return error(by, "model.connection.bad_pattern",
                       fmt::format("the {} must be a whole number from 1 to 1000", what));
        }
        return static_cast<int>(std::lround(*n));
      };
      if (const auto* bc = std::get_if<BoltCircle>(&b->holes.layout)) {
        auto n = whole(bc->count, "hole count");
        auto d = eval(bc->diameter, by, "pitch circle diameter");
        auto a0 = eval(bc->start_angle, by, "start angle");
        if (!n) return fail(n.error());
        if (!d) return fail(d.error());
        if (!a0) return fail(a0.error());
        for (int k = 0; k < *n; ++k) {
          const double a = *a0 + 2.0 * std::numbers::pi * k / *n;
          cb.hole_centres.push_back(
              cb.frame.to_model({*u + *d / 2 * std::cos(a), *v + *d / 2 * std::sin(a)}));
        }
      } else {
        const auto& g = std::get<BoltGrid>(b->holes.layout);
        auto cols = whole(g.columns, "column count");
        auto rows = whole(g.rows, "row count");
        auto px = eval(g.pitch_x, by, "x pitch");
        auto py = eval(g.pitch_y, by, "y pitch");
        if (!cols) return fail(cols.error());
        if (!rows) return fail(rows.error());
        if (!px) return fail(px.error());
        if (!py) return fail(py.error());
        for (int j = 0; j < *rows; ++j) {
          for (int i = 0; i < *cols; ++i) {
            const double x = *u + (i - (*cols - 1) / 2.0) * *px;
            const double y = *v + (j - (*rows - 1) / 2.0) * *py;
            cb.hole_centres.push_back(cb.frame.to_model({x, y}));
          }
        }
      }
    }
    cb.ok = true;
    return cb;
  }

  // --- parameters and contents ------------------------------------------------------

  void parameters(std::size_t i) {
    const Volume& v = doc_.volumes[i];
    VolumeBuild& vb = out_->volumes[i];
    vb.status = VolumeStatus::Stale;
    for (const auto& p : v.parameters) {
      const SubjectRef ps{SubjectKind::Parameter, p.id, fmt::format("{}.{}", v.name, p.name)};
      auto value = eval(p.value, ps, "value");
      if (!value) {
        vb.diagnostics.add(value.error());
        vb.status = VolumeStatus::Failed;
        continue;
      }
      vb.parameters[p.name] = *value;
    }
    if (vb.boundary.is_empty()) vb.status = VolumeStatus::Failed;
  }

 public:
  ContentsBuild contents(std::size_t i) const {
    const Volume& v = doc_.volumes[i];
    const VolumeBuild& vb = in_.volumes[i];
    const SubjectRef self = doc_.volume_subject(v.id);
    ContentsBuild cb;
    // A composite's layout was rebuilt with the layout half; keep it even
    // when the composite fails, since its children were built from it.
    if (v.composite()) cb.contents_rebuild = vb.contents_rebuild;

    if (vb.boundary.is_empty()) {
      cb.status = VolumeStatus::Failed;
      if (!v.contents_empty()) {
        cb.diagnostics.add(Diagnostic::info(
            "model.contents.no_boundary",
            fmt::format("'{}': contents not built, because the boundary failed", self.label),
            {self}));
      }
      return cb;
    }
    if (vb.status == VolumeStatus::Failed) {  // a parameter failed
      cb.status = VolumeStatus::Failed;
      return cb;
    }
    if (v.keep_out || v.contents_empty()) {
      cb.status = VolumeStatus::Current;
      return cb;
    }

    if (v.composite()) {
      // Its layout rebuilt with the layout half; its children are its
      // contents, and build() or the engine adds their measures up.
      cb.contents_rebuild = vb.contents_rebuild;
      cb.status = VolumeStatus::Current;
      return cb;
    }
    if (const auto* sc = std::get_if<StaticContents>(&v.contents)) {
      import_static(v, *sc, cb);
    } else if (const auto* pc = std::get_if<ParametricContents>(&v.contents)) {
      parametric(v, pc->tree, vb, cb);
    } else {
      cb.status = VolumeStatus::Stale;  // node graphs arrive in M10
    }
    measure(v, cb);
    return cb;
  }

 private:
  void import_static(const Volume& v, const StaticContents& sc, ContentsBuild& vb) const {
    const SubjectRef self = doc_.volume_subject(v.id);
    if (!blobs_ || (sc.format != "step" && sc.format != "iges")) {
      vb.status = VolumeStatus::Stale;  // not given the files, or a mesh (M8)
      return;
    }
    const auto it = blobs_->find(sc.blob);
    if (it == blobs_->end()) {
      vb.diagnostics.add(error(self, "model.contents.missing_blob",
                               fmt::format("its file '{}' isn't in the project", sc.blob)));
      vb.status = VolumeStatus::Failed;
      return;
    }
    // The exchange layer reads files; give it a temporary one.
    const auto tmp = std::filesystem::temp_directory_path() /
                     fmt::format("vstudio-{}.{}", Id::random().to_string(),
                                 sc.format == "step" ? "step" : "iges");
    {
      std::ofstream f(tmp, std::ios::binary);
      f.write(reinterpret_cast<const char*>(it->second->data()),
              static_cast<std::streamsize>(it->second->size()));
    }
    auto shape = sc.format == "step" ? kernel::read_step(tmp) : kernel::read_iges(tmp);
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    if (!shape) {
      vb.diagnostics.add(error(self, "model.contents.import_failed",
                               fmt::format("its file doesn't read: {}", shape.error().message)));
      vb.status = VolumeStatus::Failed;
      return;
    }
    auto solid = Solid::from(*shape);
    if (!solid) {
      const auto parts = kernel::solids(*shape);
      if (parts.size() == 1) solid = parts.front();
    }
    if (!solid) {
      vb.diagnostics.add(error(self, "model.contents.import_failed",
                               "its file holds no single solid"));
      vb.status = VolumeStatus::Failed;
      return;
    }
    vb.contents = *solid;
    vb.status = VolumeStatus::Current;
  }

  void parametric(const Volume& v, const feature::FeatureTree& tree, const VolumeBuild& layout,
                  ContentsBuild& vb) const {
    const SubjectRef self = doc_.volume_subject(v.id);
    feature::ExternalInputs inputs = inputs_for(v, layout);
    // A template's copy (L4): rebuilt where the template was authored, then
    // moved into place.
    const core::dvec3 offset = v.contents_offset;
    const bool moved = !(offset == core::dvec3{});
    if (moved) {
      for (auto& in : inputs.references) {
        feature::FeatureResult& r = in.result;
        if (r.plane) r.plane->origin = r.plane->origin - offset;
        if (r.axis) r.axis->origin = r.axis->origin - offset;
        if (r.point) *r.point = *r.point - offset;
        for (auto& f : r.faces) {
          f.plane.origin = f.plane.origin - offset;
          if (f.cylinder) f.cylinder->axis.origin = f.cylinder->axis.origin - offset;
        }
      }
    }

    feature::RebuildOptions options;
    options.inputs = &inputs;
    vb.contents_rebuild = feature::rebuild(tree, settings_, options);
    const auto& r = *vb.contents_rebuild;
    if (moved && r.ok() && !r.body.is_empty()) {
      auto placed = kernel::transformed(r.body, core::dtransform::translate(offset));
      if (!placed) {
        vb.diagnostics.add(placed.error());
        vb.status = VolumeStatus::Failed;
        return;
      }
      vb.contents = *placed;
      vb.diagnostics.append(r.diagnostics);  // warnings
      vb.status = VolumeStatus::Current;
      return;
    }
    if (!r.ok()) {
      vb.diagnostics.add(error(self, "model.contents.failed",
                               "its contents' feature tree didn't rebuild cleanly"));
      vb.diagnostics.append(r.diagnostics);
      vb.status = VolumeStatus::Failed;
    } else {
      vb.diagnostics.append(r.diagnostics);  // warnings
      vb.status = VolumeStatus::Current;
    }
    vb.contents = r.body;
  }

  void measure(const Volume& v, ContentsBuild& vb) const {
    for (const auto& o : v.outputs) {
      OutputResult res{o.id, std::nullopt};
      if (!vb.contents.is_empty()) {
        const auto mass = kernel::mass_properties(vb.contents);
        switch (o.quantity) {
          case OutputQuantity::Volume:
            if (mass) res.value = mass->volume;
            break;
          case OutputQuantity::Mass:
            // mm³ to cm³ is 1e-3; g/cm³ * cm³ = g.
            if (mass) res.value = mass->volume * 1e-3 * o.density;
            break;
          case OutputQuantity::SurfaceArea:
            res.value = kernel::surface_area(vb.contents);
            break;
          case OutputQuantity::CentreOfGravity:
            if (mass) res.value = mass->center_of_mass;
            break;
          case OutputQuantity::Envelope:
            res.value = kernel::bounding_box(vb.contents);
            break;
          case OutputQuantity::Published:
            break;  // only procedures publish numbers

        }
      }
      vb.outputs.push_back(res);
    }
  }
};

}  // namespace

DocumentBuild build_layout(const Document& document, const ProductSettings& settings) {
  DocumentBuild out;
  DocumentBuilder builder(document, settings, out);
  builder.run_layout();
  builder.collect_diagnostics();
  return out;
}

ContentsBuild build_contents(const Document& document, const DocumentBuild& layout,
                             std::size_t index, const ProductSettings& settings,
                             const BlobStore* blobs) {
  return DocumentBuilder(document, settings, blobs, layout).contents(index);
}

void apply_contents(VolumeBuild& volume, ContentsBuild contents) {
  volume.status = contents.status;
  volume.contents_rebuild = std::move(contents.contents_rebuild);
  volume.contents = std::move(contents.contents);
  volume.outputs = std::move(contents.outputs);
  volume.diagnostics.append(contents.diagnostics);
}

// --- composite volumes ---------------------------------------------------------------

void ContentsMeasure::add(const ContentsMeasure& other) {
  if (!other.any) return;
  volume += other.volume;
  moment = moment + other.moment;
  envelope.expand(other.envelope);
  any = true;
}

ContentsMeasure measure_solid(const kernel::Solid& solid) {
  ContentsMeasure m;
  if (solid.is_empty()) return m;
  const auto mass = kernel::mass_properties(solid);
  if (!mass) return m;
  m.volume = mass->volume;
  m.moment = mass->center_of_mass * mass->volume;
  m.envelope = kernel::bounding_box(solid);
  m.any = true;
  return m;
}

std::vector<OutputResult> composite_outputs(const Volume& composite,
                                            const ContentsMeasure& total) {
  std::vector<OutputResult> out;
  for (const auto& o : composite.outputs) {
    OutputResult res{o.id, std::nullopt};
    if (total.any) {
      switch (o.quantity) {
        case OutputQuantity::Volume: res.value = total.volume; break;
        case OutputQuantity::Mass: res.value = total.volume * 1e-3 * o.density; break;
        case OutputQuantity::CentreOfGravity:
          if (total.volume > 0.0) res.value = total.moment * (1.0 / total.volume);
          break;
        case OutputQuantity::Envelope: res.value = total.envelope; break;
        case OutputQuantity::SurfaceArea:
        case OutputQuantity::Published: break;  // don't add up (check() refuses them)
      }
    }
    out.push_back(res);
  }
  return out;
}

VolumeStatus worse(VolumeStatus a, VolumeStatus b) {
  const auto rank = [](VolumeStatus s) {
    switch (s) {
      case VolumeStatus::Current: return 0;
      case VolumeStatus::Regenerating: return 1;
      case VolumeStatus::Stale: return 2;
      case VolumeStatus::Failed: return 3;
    }
    return 3;
  };
  return rank(a) >= rank(b) ? a : b;
}

DocumentBuild build(const Document& document, const ProductSettings& settings,
                    const BlobStore* blobs) {
  DocumentBuild out;
  DocumentBuilder layout(document, settings, out);
  layout.run_layout();
  const DocumentBuilder contents(document, settings, blobs, out);
  std::vector<ContentsBuild> built;
  built.reserve(document.volumes.size());
  for (std::size_t i = 0; i < document.volumes.size(); ++i) built.push_back(contents.contents(i));
  for (std::size_t i = 0; i < document.volumes.size(); ++i) {
    apply_contents(out.volumes[i], std::move(built[i]));
  }
  // Composites, deepest first: their status and outputs from their
  // children's (L3).
  std::vector<ContentsMeasure> measures(document.volumes.size());
  const std::vector<std::size_t> order = level_order(document);
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const std::size_t i = *it;
    const Volume& v = document.volumes[i];
    VolumeBuild& vb = out.volumes[i];
    if (!v.composite()) {
      // A failed volume's contents (a parametric tree's body as far as it
      // got) aren't what it should hold: leave them out of the sums.
      if (vb.status != VolumeStatus::Failed) measures[i] = measure_solid(vb.contents);
      continue;
    }
    for (const std::size_t c : children(document, v.id)) {
      measures[i].add(measures[c]);
      vb.status = worse(vb.status, out.volumes[c].status);
    }
    vb.outputs = composite_outputs(v, measures[i]);
  }
  layout.collect_diagnostics();
  return out;
}

}  // namespace vs::model

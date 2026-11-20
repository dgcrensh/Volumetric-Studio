#include "vs/validate/layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include <fmt/format.h>

#include "vs/kernel/operations.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/model/hierarchy.hpp"
#include "vs/validate/registry.hpp"

namespace vs::validate {

using core::SubjectKind;
using kernel::Face;
using kernel::Solid;
using model::ConnectionBuild;
using model::Document;
using model::DocumentBuild;
using model::VolumeBuild;

namespace {

std::string mm(double v) { return fmt::format("{:.6g}", v); }

std::string box_text(const dbox3& b) {
  return fmt::format("({}, {}, {}) to ({}, {}, {})", mm(b.min.x), mm(b.min.y), mm(b.min.z),
                     mm(b.max.x), mm(b.max.y), mm(b.max.z));
}

double smallest_side(const dbox3& b) {
  const dvec3 s = b.size();
  return std::min({s.x, s.y, s.z});
}

// A boundary's faces with what's needed to pair them up quickly.
struct BoundaryFace {
  Face face;
  dbox3 box;
  std::optional<kernel::Plane> plane;
  std::string name;  // see layout.hpp
};

struct Prepared {
  const model::Volume* volume = nullptr;
  const VolumeBuild* build = nullptr;
  dbox3 box;
  std::vector<BoundaryFace> faces;  // filled on first use
  bool faces_ready = false;
};

class LayoutChecker {
 public:
  LayoutChecker(const Document& doc, const DocumentBuild& build, const ProductSettings& settings)
      : doc_(doc), build_(build), settings_(settings), tol_(settings.linear_tolerance_mm) {}

  LayoutReport run() {
    std::vector<Prepared> vols;
    for (std::size_t i = 0; i < doc_.volumes.size() && i < build_.volumes.size(); ++i) {
      const VolumeBuild& vb = build_.volumes[i];
      if (vb.boundary.is_empty()) continue;
      Prepared p;
      p.volume = &doc_.volumes[i];
      p.build = &vb;
      p.box = kernel::bounding_box(vb.boundary);
      vols.push_back(std::move(p));
    }

    Diagnostics contacts;
    for (std::size_t i = 0; i < vols.size(); ++i) {
      for (std::size_t j = i + 1; j < vols.size(); ++j) {
        Prepared& a = vols[i];
        Prepared& b = vols[j];
        // Only siblings are checked against each other (L3): a child is
        // inside its parent, and meets its parent's neighbours only
        // through surfaces delegated to it.
        if (!(a.volume->parent == b.volume->parent)) continue;
        if (!a.box.intersects(b.box, tol_)) continue;
        ++report_.pairs_tested;
        if (overlap(a, b)) continue;
        if (shares_surface(a.volume->id, b.volume->id)) continue;  // adjacent by definition
        if (a.volume->keep_out || b.volume->keep_out) continue;
        contact(a, b, contacts);
      }
    }
    report_.diagnostics.append(contacts);
    for (const Prepared& child : vols) {
      if (!child.volume->parent.is_null()) nesting(child, vols);
    }
    report_.diagnostics.append(check_connections(doc_, build_, settings_));
    return std::move(report_);
  }

 private:
  const Document& doc_;
  const DocumentBuild& build_;
  const ProductSettings& settings_;
  double tol_;
  LayoutReport report_;

  bool shares_surface(const Id& a, const Id& b) const {
    return std::any_of(doc_.connections.begin(), doc_.connections.end(),
                       [&](const model::ConnectionSurface& c) { return c.joins(a) && c.joins(b); });
  }

  void undecided(const Prepared& a, const Prepared& b, std::string_view what,
                 const Diagnostic& why) {
    report_.diagnostics.add(Diagnostic::error(
        "validate.undecided",
        fmt::format("Couldn't check '{}' and '{}' for {}: {}", a.volume->name, b.volume->name,
                    what, why.message),
        {doc_.boundary_subject(a.volume->id), doc_.boundary_subject(b.volume->id)}));
  }

  // True when the pair overlaps (reported) or couldn't be checked.
  bool overlap(const Prepared& a, const Prepared& b) {
    // The overlap lies inside both boxes, so it is no deeper than their
    // intersection's smallest side: neighbours that only touch skip the
    // boolean.
    const dbox3 meet = core::intersection(a.box, b.box);
    if (meet.is_empty() || smallest_side(meet) <= tol_) return false;

    auto common = kernel::common(a.build->boundary, b.build->boundary, {tol_});
    if (!common) {
      undecided(a, b, "overlap", common.error());
      return true;
    }
    if (common->is_empty()) return false;
    const dbox3 region = kernel::bounding_box(*common);
    const double depth = smallest_side(region);
    const auto mass = kernel::mass_properties(*common);
    const double volume = mass ? mass->volume : 0.0;
    if (depth <= tol_ || volume <= 0.0) return false;  // touching

    report_.overlaps.push_back({a.volume->id, b.volume->id, region, volume, depth});
    report_.diagnostics.add(Diagnostic::error(
        "validate.overlap",
        fmt::format("'{}' and '{}' overlap by {} mm ({} mm³ from {}); move or resize one, "
                    "or cut one with a keep-out region",
                    a.volume->name, b.volume->name, mm(depth), mm(volume), box_text(region)),
        {doc_.boundary_subject(a.volume->id), doc_.boundary_subject(b.volume->id)}));
    return true;
  }

  // --- nesting (L3) ----------------------------------------------------------------

  // A child's boundary must lie inside its composite's: whatever pokes out
  // is reported with its depth (the region's thinnest side), as overlap is.
  void nesting(const Prepared& child, const std::vector<Prepared>& vols) {
    const auto parent = std::find_if(vols.begin(), vols.end(), [&](const Prepared& p) {
      return p.volume->id == child.volume->parent;
    });
    if (parent == vols.end()) return;  // the parent's boundary failed: reported already
    if (parent->box.contains(child.box, tol_)) {
      // Inside the box, but the boundary needn't be a box: check properly
      // unless the parent fills its box.
      const auto pm = kernel::mass_properties(parent->build->boundary);
      if (pm && std::abs(pm->volume - parent->box.volume()) <= tol_ * parent->box.volume()) return;
    }
    auto outside = kernel::cut(child.build->boundary, std::vector<Solid>{parent->build->boundary}, {tol_});
    if (!outside) {
      if (outside.error().code == "kernel.boolean.empty_result") return;
      undecided(child, *parent, "nesting", outside.error());
      return;
    }
    if (outside->is_empty()) return;
    const dbox3 region = kernel::bounding_box(*outside);
    const double depth = smallest_side(region);
    const auto mass = kernel::mass_properties(*outside);
    const double volume = mass ? mass->volume : 0.0;
    if (depth <= tol_ || volume <= 0.0) return;
    report_.diagnostics.add(Diagnostic::error(
        "validate.nesting.outside",
        fmt::format("'{}' reaches {} mm outside '{}', the composite it belongs to ({} mm³ from "
                    "{}); a child must stay inside its parent's boundary",
                    child.volume->name, mm(depth), parent->volume->name, mm(volume),
                    box_text(region)),
        {doc_.boundary_subject(child.volume->id), doc_.boundary_subject(parent->volume->id)}));
  }

  // --- contact --------------------------------------------------------------------

  std::string face_label(const Prepared& p, const Face& face, std::size_t index) const {
    const auto plane = kernel::face_plane(face);
    const auto cylinder = plane ? std::nullopt : kernel::face_cylinder(face);
    for (const auto& n : p.build->boundary_faces) {
      bool same = false;
      if (plane && !n.cylinder) {
        same = dot(plane->normal, n.plane.normal) > 1.0 - 1e-9 &&
               std::abs(dot(plane->origin - n.plane.origin, n.plane.normal)) <= tol_;
      } else if (cylinder && n.cylinder) {
        const auto& c = *n.cylinder;
        const dvec3 offset = cylinder->axis.origin - c.axis.origin;
        const dvec3 across = offset - c.axis.direction * dot(offset, c.axis.direction);
        same = std::abs(cylinder->radius - c.radius) <= tol_ &&
               std::abs(dot(cylinder->axis.direction, c.axis.direction)) > 1.0 - 1e-9 &&
               length(across) <= tol_ && cylinder->inward == c.inward;
      }
      if (same) return readable(n.name, p.volume->parent);
    }
    return fmt::format("face {}", index + 1);
  }

  // "<feature id>:<name>" with the feature's label instead of its id; the
  // feature is in the layout of the volume's level.
  std::string readable(const std::string& name, const Id& level) const {
    const auto colon = name.find(':');
    if (colon == std::string::npos) return name;
    const auto id = Id::parse(name.substr(0, colon));
    if (!id) return name;
    const feature::FeatureTree* layout = model::level_layout(doc_, level);
    return fmt::format("{}:{}", layout != nullptr ? layout->label(*id) : id->to_string(),
                       name.substr(colon + 1));
  }

  const std::vector<BoundaryFace>& faces_of(Prepared& p) const {
    if (!p.faces_ready) {
      const auto list = kernel::faces(p.build->boundary);
      for (std::size_t k = 0; k < list.size(); ++k) {
        p.faces.push_back({list[k], kernel::bounding_box(list[k]), kernel::face_plane(list[k]),
                           face_label(p, list[k], k)});
      }
      p.faces_ready = true;
    }
    return p.faces;
  }

  SubjectRef face_subject(const Prepared& p, const std::string& face) const {
    SubjectRef s = doc_.boundary_subject(p.volume->id);
    s.label = fmt::format("{}, face '{}'", s.label, face);
    return s;
  }

  void contact(Prepared& a, Prepared& b, Diagnostics& out) {
    auto dist = kernel::min_distance(a.build->boundary, b.build->boundary);
    if (!dist) {
      undecided(a, b, "contact", dist.error());
      return;
    }
    if (dist->distance > tol_) return;

    const double min_area = settings_.min_feature_size_mm() * settings_.min_feature_size_mm();
    bool any = false;
    for (const auto& fa : faces_of(a)) {
      for (const auto& fb : faces_of(b)) {
        if (!fa.box.intersects(fb.box, tol_)) continue;
        if (fa.plane && fb.plane) {
          // Planar faces lie against each other only when they face each
          // other on one plane.
          if (dot(fa.plane->normal, fb.plane->normal) > -1.0 + 1e-9) continue;
          if (std::abs(dot(fb.plane->origin - fa.plane->origin, fa.plane->normal)) > tol_) continue;
        }
        auto shared = kernel::common_faces(fa.face, fb.face, {tol_});
        if (!shared) {
          undecided(a, b, "contact", shared.error());
          return;
        }
        if (shared->is_empty()) continue;
        const double area = kernel::surface_area(*shared);
        if (area <= min_area) continue;
        any = true;
        const dbox3 region = kernel::bounding_box(*shared);
        report_.contacts.push_back({a.volume->id, b.volume->id, fa.name, fb.name, area, region});
        out.add(Diagnostic::warning(
            "validate.contact",
            fmt::format("Face '{}' of '{}' touches face '{}' of '{}' over {} mm², with no "
                        "connection surface between them; add one, or move them apart",
                        fa.name, a.volume->name, fb.name, b.volume->name, mm(area)),
            {face_subject(a, fa.name), face_subject(b, fb.name)}));
      }
    }
    if (any) return;

    dbox3 region = dbox3::empty();
    region.expand(dist->point_on_a);
    region.expand(dist->point_on_b);
    report_.contacts.push_back({a.volume->id, b.volume->id, {}, {}, 0.0, region});
    out.add(Diagnostic::warning(
        "validate.contact.edge",
        fmt::format("'{}' and '{}' touch along an edge or at a point near ({}, {}, {}), with no "
                    "connection surface between them; move them apart or let them meet face "
                    "to face through a connection surface",
                    a.volume->name, b.volume->name, mm(dist->point_on_a.x),
                    mm(dist->point_on_a.y), mm(dist->point_on_a.z)),
        {doc_.boundary_subject(a.volume->id), doc_.boundary_subject(b.volume->id)}));
  }
};

// --- connection rules ------------------------------------------------------------

std::string params_text(std::initializer_list<std::string_view> names) {
  std::string out;
  for (auto n : names) {
    if (!out.empty()) out += ", ";
    out += n;
  }
  return out;
}

void check_bolts(const Document& doc, const model::ConnectionSurface& c,
                 const model::BoltedFlangeParams& params, const ConnectionBuild& cb,
                 const ProductSettings& settings, Diagnostics& out) {
  const double tol = settings.linear_tolerance_mm;
  const SubjectRef self = doc.connection_subject(c.id);
  const double wall = cb.min_wall.value_or(0.0);
  if (wall < settings.min_feature_size_mm()) {
    out.add(Diagnostic::error(
        "validate.connection.min_wall",
        fmt::format("'{}': the minimum wall is {} mm; it must be at least {} mm (parameters: {})",
                    c.name, mm(wall), mm(settings.min_feature_size_mm()),
                    params_text({"min_wall"})),
        {self}));
    return;
  }
  const auto thread = model::thread_size(params.thread);
  if (!thread) return;  // model::check() reports unknown threads
  const double r = thread->clearance_hole / 2.0;

  // Every hole inside the patch with the wall to its edge.
  const auto edges = kernel::edges(cb.patch);
  std::optional<std::size_t> worst;
  double worst_room = std::numeric_limits<double>::infinity();
  std::size_t failing = 0;
  for (std::size_t k = 0; k < cb.hole_centres.size(); ++k) {
    const dvec3& h = cb.hole_centres[k];
    auto on = kernel::distance_to(cb.patch, h);
    double room = std::numeric_limits<double>::infinity();
    for (const auto& e : edges) {
      auto d = kernel::distance_to(e, h);
      if (d) room = std::min(room, *d);
    }
    if (!on || *on > tol) room = -room;  // the centre is off the patch
    if (room < r + wall - tol) {
      ++failing;
      if (room < worst_room) {
        worst_room = room;
        worst = k;
      }
    }
  }
  if (worst) {
    const std::string where =
        worst_room < 0 ? "lies off the patch"
                       : fmt::format("is {} mm from the patch's edge", mm(worst_room));
    out.add(Diagnostic::error(
        "validate.connection.edge_wall",
        fmt::format("'{}': hole {} of {} {}, but an {} clearance hole ({} mm) with a {} mm "
                    "minimum wall needs its centre {} mm inside the edge ({} hole(s) too close; "
                    "parameters: {})",
                    c.name, *worst + 1, cb.hole_centres.size(), where, thread->name,
                    mm(thread->clearance_hole), mm(wall), mm(r + wall), failing,
                    params_text({"holes", "thread", "min_wall"})),
        {self}));
  }

  // Wall between neighbouring holes.
  double closest = std::numeric_limits<double>::infinity();
  std::pair<std::size_t, std::size_t> pair{0, 0};
  for (std::size_t i = 0; i < cb.hole_centres.size(); ++i) {
    for (std::size_t j = i + 1; j < cb.hole_centres.size(); ++j) {
      const double d = core::distance(cb.hole_centres[i], cb.hole_centres[j]);
      if (d < closest) {
        closest = d;
        pair = {i, j};
      }
    }
  }
  if (closest < 2.0 * r + wall - tol) {
    out.add(Diagnostic::error(
        "validate.connection.hole_spacing",
        fmt::format("'{}': holes {} and {} are {} mm apart, but two {} clearance holes ({} mm) "
                    "with a {} mm wall between them need {} mm (parameters: {})",
                    c.name, pair.first + 1, pair.second + 1, mm(closest), thread->name,
                    mm(thread->clearance_hole), mm(wall), mm(2.0 * r + wall),
                    params_text({"holes", "thread", "min_wall"})),
        {self}));
  }
}

}  // namespace

Diagnostics check_connections(const Document& document, const DocumentBuild& build,
                              const ProductSettings& settings) {
  Diagnostics out;
  const double tol = settings.linear_tolerance_mm;
  for (const auto& c : document.connections) {
    const ConnectionBuild* cb = build.find_connection(c.id);
    if (!cb || !cb->ok) continue;
    const SubjectRef self = document.connection_subject(c.id);

    // Clearance gap first: the boundary rule below depends on it.
    double reach = tol;
    if (c.type() == model::ConnectionType::Clearance) {
      const double gap = cb->gap.value_or(0.0);
      if (gap < settings.min_feature_size_mm()) {
        out.add(Diagnostic::error(
            "validate.connection.gap",
            fmt::format("'{}': the clearance gap is {} mm; it must be at least {} mm "
                        "(parameters: {})",
                        c.name, mm(gap), mm(settings.min_feature_size_mm()),
                        params_text({"gap"})),
            {self}));
      } else {
        reach += gap;
      }
    }

    // Each volume it names, and each it is delegated down to (L3).
    std::vector<Id> met;
    for (const auto& named : c.volumes) {
      Id at = named;
      for (std::size_t guard = 0; !at.is_null() && guard <= document.volumes.size(); ++guard) {
        met.push_back(at);
        at = model::delegate_of(document, at, c.id);
      }
    }
    for (const auto& vid : met) {
      const VolumeBuild* vb = build.find_volume(vid);
      if (!vb || vb->boundary.is_empty()) continue;
      auto d = kernel::min_distance(cb->patch, vb->boundary);
      if (!d) {
        out.add(Diagnostic::error(
            "validate.undecided",
            fmt::format("Couldn't check that '{}' lies on '{}': {}", c.name,
                        document.volume_subject(vid).label, d.error().message),
            {self, document.boundary_subject(vid)}));
        continue;
      }
      if (d->distance > reach) {
        const bool clearance = c.type() == model::ConnectionType::Clearance;
        out.add(Diagnostic::error(
            "validate.connection.off_boundary",
            fmt::format("'{}' is {} mm from the boundary of '{}', which it joins; a {} surface "
                        "must lie on {} (parameters: geometry{})",
                        c.name, mm(d->distance), document.volume_subject(vid).label,
                        to_string(c.type()),
                        clearance ? "it, or within the gap of it" : "it",
                        clearance ? ", gap" : ""),
            {self, document.boundary_subject(vid)}));
      }
    }

    if (const auto* bolts = std::get_if<model::BoltedFlangeParams>(&c.params)) {
      check_bolts(document, c, *bolts, *cb, settings, out);
    }
  }
  return out;
}

LayoutReport validate_layout(const Document& document, const DocumentBuild& build,
                             const ProductSettings& settings) {
  return LayoutChecker(document, build, settings).run();
}

}  // namespace vs::validate

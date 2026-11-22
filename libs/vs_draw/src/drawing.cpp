#include "vs/draw/drawing.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

#include <fmt/format.h>

#include "vs/kernel/queries.hpp"

namespace vs::draw {

using core::Diagnostic;
using core::Diagnostics;
using core::SubjectKind;
using core::SubjectRef;
using model::Anchor;
using model::AnchorOn;
using model::AnchorPart;
using model::DimensionKind;

const DimensionResult* DrawingResult::find(const Id& dimension) const {
  const auto it = std::find_if(dimensions.begin(), dimensions.end(),
                               [&](const DimensionResult& d) { return d.dimension == dimension; });
  return it == dimensions.end() ? nullptr : &*it;
}

std::string number_text(double value, int decimals) {
  decimals = std::clamp(decimals, 0, 6);
  const double scale = std::pow(10.0, decimals);
  double r = std::round(value * scale) / scale;
  if (r == 0.0) r = 0.0;
  std::string s = fmt::format("{:.{}f}", r, decimals);
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  if (s == "-0") s = "0";
  return s;
}

namespace {

constexpr double kScales[] = {50, 20, 10, 5, 2, 1, 0.5, 0.2, 0.1, 0.05, 0.02, 0.01, 0.005, 0.002, 0.001};

}  // namespace

double standard_scale(double fit) {
  for (const double s : kScales) {
    if (s <= fit * (1 + 1e-12)) return s;
  }
  return kScales[std::size(kScales) - 1];
}

std::string scale_text(double scale) {
  if (!(scale > 0)) return "?";
  if (scale >= 1) return number_text(scale, 3) + ":1";
  return "1:" + number_text(1.0 / scale, 3);
}

namespace {

// Text, arrows and lines of a dimension, in sheet mm.
constexpr double kArrow = 3.0;
constexpr double kArrowHalf = 0.5;
constexpr double kGap = 1.0;       // between the geometry and an extension line
constexpr double kOvershoot = 2.0; // of an extension line past the dimension line
constexpr double kParallel = 1e-9; // 1 - |cos| below which directions are parallel

bool parallel(const dvec3& a, const dvec3& b) {
  const double l = core::length(a) * core::length(b);
  return l > 0 && std::abs(dot(a, b)) >= (1 - kParallel) * l;
}

struct Failure {
  std::string code;  // draw.dimension.*
  std::string why;
  SubjectRef subject;
};

struct Located {
  double along = 0;  // along the dimension's direction, mm
  dvec3 at{};        // where its extension line starts
};

struct Circle {
  dvec3 centre{};
  double diameter = 0;
};

std::string sheet_name(double w, double h) {
  struct Size {
    const char* name;
    double a, b;
  };
  constexpr Size kSizes[] = {{"A4", 297, 210}, {"A3", 420, 297}, {"A2", 594, 420},
                             {"A1", 841, 594}, {"A0", 1189, 841}};
  for (const auto& s : kSizes) {
    if ((w == s.a && h == s.b) || (w == s.b && h == s.a)) return s.name;
  }
  return fmt::format("{} x {} mm", number_text(w, 1), number_text(h, 1));
}

class Drafter {
 public:
  Drafter(const eval::Evaluation& ev, const model::Drawing& drawing, const DrawOptions& options)
      : ev_(ev), doc_(ev.document), drawing_(drawing), options_(options) {
    for (std::size_t i = 0; i < ev_.volumes.size(); ++i) results_[ev_.volumes[i].volume] = i;
    for (const auto& v : doc_.volumes) {
      if (!v.parent.is_null()) children_[v.parent].push_back(v.id);
    }
  }

  DrawingResult run() {
    out_.sheet.width = drawing_.width;
    out_.sheet.height = drawing_.height;
    out_.sheet.title = drawing_.name;
    frame_and_title();
    for (const auto& v : drawing_.views) view(v);
    for (std::size_t i = 0; i < drawing_.dimensions.size(); ++i) dimension(i);
    labels();
    return std::move(out_);
  }

 private:
  const eval::Evaluation& ev_;
  const model::Document& doc_;
  const model::Drawing& drawing_;
  const DrawOptions& options_;
  DrawingResult out_;
  std::unordered_map<Id, std::size_t> results_;
  std::unordered_map<Id, std::vector<Id>> children_;
  std::set<Id> warned_stale_;
  // Per view: where it sits.
  struct Placed {
    ViewFrame frame;
    double scale = 1;
    dvec2 centre_model{};  // in view mm
    dvec2 centre_sheet{};
  };
  std::map<Id, Placed> placed_;
  std::map<Id, double> lowest_;  // per view: the lowest sheet y drawn
  std::unordered_map<Id, std::vector<dvec3>> points_;  // tessellated points by volume / contents

  // --- subjects ------------------------------------------------------------------

  SubjectRef item_subject(const Id& item) const {
    const std::string name = drawing_.name.empty() ? "(unnamed)" : drawing_.name;
    if (const auto* v = drawing_.find_view(item)) {
      return {SubjectKind::Drawing, item, fmt::format("view '{}' of drawing '{}'", v->name, name)};
    }
    for (std::size_t i = 0; i < drawing_.dimensions.size(); ++i) {
      if (drawing_.dimensions[i].id == item) {
        return {SubjectKind::Drawing, item, fmt::format("dimension {} of drawing '{}'", i + 1, name)};
      }
    }
    return {SubjectKind::Drawing, drawing_.id, fmt::format("drawing '{}'", name)};
  }

  // --- lookups ------------------------------------------------------------------

  const eval::VolumeResult* result(const Id& volume) const {
    const auto it = results_.find(volume);
    return it == results_.end() ? nullptr : &ev_.volumes[it->second];
  }

  // The volume and everything inside it, parents first.
  void collect(const Id& volume, std::vector<Id>& out) const {
    out.push_back(volume);
    const auto it = children_.find(volume);
    if (it == children_.end()) return;
    for (const auto& c : it->second) collect(c, out);
  }

  std::vector<Id> shown(const model::DrawingView& v) const {
    std::vector<Id> out;
    if (v.volumes.empty()) {
      for (const auto& vol : doc_.volumes) out.push_back(vol.id);
      return out;
    }
    std::set<Id> seen;
    for (const auto& id : v.volumes) {
      if (!doc_.find_volume(id)) continue;
      std::vector<Id> some;
      collect(id, some);
      for (const auto& s : some) {
        if (seen.insert(s).second) out.push_back(s);
      }
    }
    return out;
  }

  // Leaves at or under `volume` with contents to show.
  std::vector<std::pair<Id, const eval::GeneratedContents*>> contents_under(const Id& volume) const {
    std::vector<Id> all;
    collect(volume, all);
    std::vector<std::pair<Id, const eval::GeneratedContents*>> out;
    for (const auto& id : all) {
      const auto* r = result(id);
      if (!r || !r->contents) continue;
      const auto& c = *r->contents;
      if (c.solid.is_empty() && (!c.mesh || c.mesh->empty())) continue;
      out.emplace_back(id, &c);
    }
    return out;
  }

  const kernel::Solid* boundary(const Id& volume) const {
    const auto* vb = ev_.build.find_volume(volume);
    if (!vb || vb->boundary.is_empty()) return nullptr;
    return &vb->boundary;
  }

  double model_deflection() const { return std::max(options_.deflection, 1e-4); }

  const std::vector<dvec3>& boundary_points(const Id& volume) {
    const auto it = points_.find(volume);
    if (it != points_.end()) return it->second;
    std::vector<dvec3> pts;
    if (const auto* b = boundary(volume)) {
      pts = shape_points(*b, model_deflection(), options_.settings);
    }
    return points_[volume] = std::move(pts);
  }

  std::vector<dvec3> contents_points(const Id& volume) {
    std::vector<dvec3> out;
    for (const auto& [id, c] : contents_under(volume)) {
      const Id key = Id::derive(id, "contents points");
      auto it = points_.find(key);
      if (it == points_.end()) {
        std::vector<dvec3> pts;
        if (!c->solid.is_empty()) pts = shape_points(c->solid, model_deflection(), options_.settings);
        if (c->mesh) {
          for (const auto& p : c->mesh->positions) pts.push_back(core::to_fp64(p));
        }
        it = points_.emplace(key, std::move(pts)).first;
      }
      out.insert(out.end(), it->second.begin(), it->second.end());
    }
    return out;
  }

  // --- sheet ------------------------------------------------------------------------

  void path(Pen pen, const Id& source, std::vector<dvec2> points, bool closed = false,
            bool filled = false) {
    out_.sheet.paths.push_back({pen, source, std::move(points), closed, filled});
  }
  void text(Pen pen, const Id& source, dvec2 at, double height, std::string s,
            TextAlign align = TextAlign::Left, bool vertical = false) {
    out_.sheet.texts.push_back({pen, source, at, height, vertical, align, std::move(s)});
  }

  void frame_and_title() {
    const double w = drawing_.width;
    const double h = drawing_.height;
    const double m = std::min({10.0, w / 20, h / 20});
    path(Pen::Border, {}, {{m, m}, {w - m, m}, {w - m, h - m}, {m, h - m}}, true);
    const double tw = std::min(180.0, w - 2 * m);
    const double row = std::min(8.0, (h - 2 * m) / 8);
    const double th = 4 * row;
    const double x0 = w - m - tw;
    const double y0 = m;
    path(Pen::Border, {}, {{x0, y0}, {x0 + tw, y0}, {x0 + tw, y0 + th}, {x0, y0 + th}}, true);
    for (int r = 1; r < 4; ++r) {
      path(Pen::Border, {}, {{x0, y0 + r * row}, {x0 + tw, y0 + r * row}});
    }
    const double small = row * 0.32;
    const double pad = 2;
    // Top row: the drawing's name.
    text(Pen::Border, {}, {x0 + pad, y0 + 3 * row + row * 0.28}, row * 0.55,
         drawing_.name.empty() ? std::string("(unnamed)") : drawing_.name);
    // Document and configuration.
    path(Pen::Border, {}, {{x0 + tw / 2, y0 + row}, {x0 + tw / 2, y0 + 3 * row}});
    const double base = row * 0.35;
    text(Pen::Border, {}, {x0 + pad, y0 + 2 * row + base}, small,
         fmt::format("Document  {}", doc_.name));
    text(Pen::Border, {}, {x0 + tw / 2 + pad, y0 + 2 * row + base}, small,
         fmt::format("Configuration  {}",
                     ev_.configuration_name.empty() ? std::string("(authored)")
                                                    : ev_.configuration_name));
    // Scale, units and sheet.
    std::set<double> scales;
    for (const auto& v : drawing_.views) scales.insert(v.scale);
    const std::string scale = scales.empty()      ? std::string("-")
                              : scales.size() == 1 ? scale_text(*scales.begin())
                                                   : std::string("as marked");
    text(Pen::Border, {}, {x0 + pad, y0 + row + base}, small, fmt::format("Scale  {}", scale));
    text(Pen::Border, {}, {x0 + tw / 2 + pad, y0 + row + base}, small,
         fmt::format("Units  mm    Sheet  {}", sheet_name(w, h)));
    text(Pen::Border, {}, {x0 + pad, y0 + base}, small,
         "Dimensions in parentheses are on generated contents and regenerate with them");
  }

  // --- views ------------------------------------------------------------------------

  void view(const model::DrawingView& v) {
    const SubjectRef self = item_subject(v.id);
    const ViewFrame frame = view_frame(v.direction);
    const double scale = v.scale > 0 ? v.scale : 1.0;
    ProjectionOptions po;
    po.deflection = options_.deflection / scale;
    po.step = options_.step / scale;
    po.simplify = options_.simplify / scale;
    po.parallel = options_.parallel;
    ViewProjector projector(frame, po, options_.settings);

    const std::vector<Id> ids = shown(v);
    const std::set<Id> shown_set(ids.begin(), ids.end());

    // Where it sits: the centre of the shown boundaries, else contents.
    dvec2 lo{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    dvec2 hi = -lo;
    auto grow = [&](const std::vector<dvec3>& pts) {
      for (const auto& p : pts) {
        const dvec2 q = frame.project(p);
        lo = core::min(lo, q);
        hi = core::max(hi, q);
      }
    };
    for (const auto& id : ids) grow(boundary_points(id));
    if (!(lo.x <= hi.x)) {
      for (const auto& id : ids) grow(contents_points(id));
    }
    const bool any = lo.x <= hi.x;
    const dvec2 centre = any ? (lo + hi) * 0.5 : dvec2{};
    placed_[v.id] = {frame, scale, centre, {v.x, v.y}};

    std::size_t drawn_items = 0;
    if (v.boundaries) {
      for (const auto& id : ids) {
        if (const auto* b = boundary(id)) {
          projector.add_outline(id, *b);
          ++drawn_items;
        }
      }
    }
    if (v.surfaces) {
      for (const auto& c : doc_.connections) {
        const bool joins = std::any_of(c.volumes.begin(), c.volumes.end(),
                                       [&](const Id& id) { return shown_set.contains(id); });
        if (!joins) continue;
        const auto* cb = ev_.build.find_connection(c.id);
        if (!cb || !cb->ok || cb->patch.is_empty()) continue;
        projector.add_outline(c.id, cb->patch);
        ++drawn_items;
      }
    }
    if (v.contents) {
      for (const auto& id : ids) {
        const model::Volume* vol = doc_.find_volume(id);
        if (!vol || vol->composite() || vol->keep_out || vol->contents_empty()) continue;
        const auto* r = result(id);
        if (r && r->contents && (!r->contents->solid.is_empty() || r->contents->mesh)) {
          if (r->contents->mesh) {
            projector.add_contents(id, *r->contents->mesh);
          } else {
            projector.add_contents(id, r->contents->solid);
          }
          ++drawn_items;
        }
        stale(id, r);
      }
    }
    if (drawn_items == 0) {
      out_.diagnostics.add(Diagnostic::warning(
          "draw.view.empty", fmt::format("'{}': it shows nothing that built", self.label), {self}));
    }

    const ProjectedView pv = projector.run(v.hidden);
    out_.diagnostics.append(pv.diagnostics);
    ViewResult vr{v.id, 0, 0, 0};
    dvec2 slo{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    dvec2 shi = -slo;
    for (const auto& line : pv.lines) {
      std::vector<dvec2> pts;
      pts.reserve(line.points.size());
      for (const auto& q : line.points) {
        const dvec2 s = to_sheet(v.id, q);
        slo = core::min(slo, s);
        shi = core::max(shi, s);
        pts.push_back(s);
      }
      Pen pen = Pen::Visible;
      switch (line.kind) {
        case LineKind::Outline:
          pen = doc_.find_connection(line.source) ? Pen::Surface : Pen::Boundary;
          ++vr.outline_lines;
          break;
        case LineKind::Visible:
          pen = Pen::Visible;
          ++vr.visible_lines;
          break;
        case LineKind::Hidden:
          pen = Pen::Hidden;
          ++vr.hidden_lines;
          break;
      }
      path(pen, line.source, std::move(pts));
    }
    out_.views.push_back(vr);
    // The label goes under the view and its dimensions (labels()).
    lowest_[v.id] = slo.y <= shi.y ? slo.y : v.y;
  }

  // Each view's label, under everything drawn for it.
  void labels() {
    for (const auto& p : out_.sheet.paths) {
      const auto* d = drawing_.find_dimension(p.source);
      if (!d) continue;
      auto it = lowest_.find(d->view);
      if (it == lowest_.end()) continue;
      for (const auto& q : p.points) it->second = std::min(it->second, q.y);
    }
    for (const auto& t : out_.sheet.texts) {
      const auto* d = drawing_.find_dimension(t.source);
      if (!d) continue;
      auto it = lowest_.find(d->view);
      if (it == lowest_.end()) continue;
      it->second = std::min(it->second, t.vertical ? t.at.y - text_width(t.text, t.height) : t.at.y);
    }
    for (const auto& v : drawing_.views) {
      const auto it = lowest_.find(v.id);
      if (it == lowest_.end()) continue;
      text(Pen::Dimension, v.id, {v.x, it->second - 2 - options_.text_height},
           options_.text_height,
           fmt::format("{}  {}",
                       v.name.empty() ? std::string(model::to_string(v.direction)) : v.name,
                       scale_text(v.scale > 0 ? v.scale : 1.0)),
           TextAlign::Centre);
    }
  }

  void stale(const Id& volume, const eval::VolumeResult* r) {
    const bool current = r && r->status == model::VolumeStatus::Current && !r->stale_contents;
    if (current || !warned_stale_.insert(volume).second) return;
    const SubjectRef vs = doc_.volume_subject(volume);
    std::string why = !r || !r->contents ? "has no contents to draw"
                                         : "isn't current: its last valid contents are drawn";
    out_.diagnostics.add(Diagnostic::warning(
        "draw.contents.stale", fmt::format("'{}' {}", vs.label, why), {vs}));
  }

  dvec2 to_sheet(const Id& view, const dvec2& q) const {
    const Placed& p = placed_.at(view);
    return p.centre_sheet + (q - p.centre_model) * p.scale;
  }

  // --- anchors ------------------------------------------------------------------------

  // The location among `pts` furthest along `m` (ties: furthest along
  // `prefer`).
  static std::optional<dvec3> extreme(const std::vector<dvec3>& pts, const dvec3& m, bool max,
                                      const dvec3& prefer) {
    if (pts.empty()) return std::nullopt;
    const double sign = max ? 1.0 : -1.0;
    double best = -std::numeric_limits<double>::infinity();
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (const auto& p : pts) {
      const double a = dot(p, m);
      lo = std::min(lo, a);
      hi = std::max(hi, a);
      best = std::max(best, sign * a);
    }
    const double tol = 1e-6 * std::max(1.0, hi - lo);
    std::optional<dvec3> out;
    double pref = -std::numeric_limits<double>::infinity();
    for (const auto& p : pts) {
      if (sign * dot(p, m) < best - tol) continue;
      const double q = dot(p, prefer);
      if (q > pref) {
        pref = q;
        out = p;
      }
    }
    return out;
  }

  // The faces of `solid` lying on `plane`: the centre of their box.
  static dvec3 face_location(const kernel::Shape& solid, const kernel::Plane& plane) {
    core::dbox3 box;
    for (const auto& f : kernel::faces(solid)) {
      const auto p = kernel::face_surface_plane(f);
      if (!p || !parallel(p->normal, plane.normal)) continue;
      if (std::abs(dot(p->origin - plane.origin, plane.normal)) > 1e-6) continue;
      box.expand(kernel::bounding_box(f));
    }
    return box.is_empty() ? plane.origin : box.center();
  }

  struct FaceFound {
    const feature::NamedFace* face = nullptr;
    const kernel::Shape* solid = nullptr;
  };

  std::optional<FaceFound> find_face(const Anchor& a, Failure& f) {
    if (a.on == AnchorOn::VolumeBoundary) {
      const auto* vb = ev_.build.find_volume(a.target);
      if (!vb || vb->boundary.is_empty()) {
        f = {"draw.dimension.unresolved", "its boundary didn't build", doc_.volume_subject(a.target)};
        return std::nullopt;
      }
      for (const auto& nf : vb->boundary_faces) {
        if (nf.name == a.face) return FaceFound{&nf, &vb->boundary};
      }
      f = {"draw.dimension.unresolved", fmt::format("its boundary has no face '{}'", a.face),
           doc_.volume_subject(a.target)};
      return std::nullopt;
    }
    // Contents: a parametric volume's own features.
    const auto* r = result(a.target);
    if (!r || !r->contents || !r->contents->rebuild || r->contents->solid.is_empty()) {
      f = {"draw.dimension.lost", "its contents have no named faces now",
           doc_.volume_subject(a.target)};
      return std::nullopt;
    }
    for (const auto& fr : r->contents->rebuild->features) {
      const std::string prefix = fr.feature.to_string() + ":";
      if (!a.face.starts_with(prefix)) continue;
      for (const auto& nf : fr.faces) {
        if (a.face.size() == prefix.size() + nf.name.size() &&
            a.face.compare(prefix.size(), std::string::npos, nf.name) == 0) {
          return FaceFound{&nf, &r->contents->solid};
        }
      }
    }
    f = {"draw.dimension.lost", fmt::format("its contents have no face '{}' now", a.face),
         doc_.volume_subject(a.target)};
    return std::nullopt;
  }

  const model::ConnectionBuild* surface_build(const Anchor& a, Failure& f) const {
    const auto* cb = ev_.build.find_connection(a.target);
    if (!cb || !cb->ok || cb->patch.is_empty()) {
      f = {"draw.dimension.unresolved", "its patch didn't build", doc_.connection_subject(a.target)};
      return nullptr;
    }
    return cb;
  }

  // An anchor's position along `m` (a unit vector in the view plane).
  std::optional<Located> locate(const Anchor& a, const dvec3& m, const dvec3& prefer,
                                const ViewFrame& frame, Failure& f) {
    switch (a.on) {
      case AnchorOn::Origin:
        return Located{0.0, {}};
      case AnchorOn::VolumeBoundary:
      case AnchorOn::VolumeContents: {
        const bool contents = a.on == AnchorOn::VolumeContents;
        const SubjectRef vs = doc_.volume_subject(a.target);
        if (a.part == AnchorPart::Face) {
          const auto found = find_face(a, f);
          if (!found) return std::nullopt;
          const auto& nf = *found->face;
          if (nf.cylinder) {
            if (!parallel(nf.cylinder->axis.direction, frame.toward)) {
              f = {"draw.dimension.not_parallel",
                   fmt::format("face '{}' is a cylinder whose axis doesn't face the viewer", a.face),
                   vs};
              return std::nullopt;
            }
            return Located{dot(m, nf.cylinder->axis.origin), nf.cylinder->axis.origin};
          }
          if (!parallel(nf.plane.normal, m)) {
            f = {"draw.dimension.not_parallel",
                 fmt::format("face '{}' doesn't face along the dimension", a.face), vs};
            return std::nullopt;
          }
          return Located{dot(m, nf.plane.origin), face_location(*found->solid, nf.plane)};
        }
        std::vector<dvec3> pts;
        std::vector<const kernel::Shape*> solids;
        if (contents) {
          for (const auto& [id, c] : contents_under(a.target)) {
            if (!c->solid.is_empty()) solids.push_back(&c->solid);
          }
          pts = contents_points(a.target);
          if (pts.empty()) {
            f = {"draw.dimension.lost", "it has no contents now", vs};
            return std::nullopt;
          }
        } else {
          const auto* b = boundary(a.target);
          if (!b) {
            f = {"draw.dimension.unresolved", "its boundary didn't build", vs};
            return std::nullopt;
          }
          solids.push_back(b);
          pts = boundary_points(a.target);
        }
        if (a.part == AnchorPart::Centre) {
          core::dbox3 box;
          for (const auto* s : solids) box.expand(kernel::bounding_box(*s));
          if (contents) {
            for (const auto& [id, c] : contents_under(a.target)) {
              if (!c->mesh) continue;
              for (const auto& q : c->mesh->positions) box.expand(core::to_fp64(q));
            }
          }
          if (box.is_empty()) {
            f = {contents ? "draw.dimension.lost" : "draw.dimension.unresolved", "it has no box",
                 vs};
            return std::nullopt;
          }
          return Located{dot(m, box.center()), box.center()};
        }
        const bool max = a.part == AnchorPart::Max;
        const auto at = extreme(pts, m, max, prefer);
        if (!at) {
          f = {contents ? "draw.dimension.lost" : "draw.dimension.unresolved", "it has no points",
               vs};
          return std::nullopt;
        }
        // Exact for B-rep: the kernel's extreme; meshes' vertices are exact.
        double along = dot(m, *at);
        if (!solids.empty()) {
          double best = max ? -std::numeric_limits<double>::infinity()
                            : std::numeric_limits<double>::infinity();
          bool have = false;
          for (const auto* s : solids) {
            if (const auto e = kernel::extent(*s, m)) {
              best = max ? std::max(best, e->second) : std::min(best, e->first);
              have = true;
            }
          }
          if (contents) {
            for (const auto& [id, c] : contents_under(a.target)) {
              if (!c->mesh) continue;
              for (const auto& p : c->mesh->positions) {
                const double d = dot(m, core::to_fp64(p));
                best = max ? std::max(best, d) : std::min(best, d);
                have = true;
              }
            }
          }
          if (have) along = best;
        }
        return Located{along, *at};
      }
      case AnchorOn::Surface: {
        const auto* cb = surface_build(a, f);
        if (!cb) return std::nullopt;
        const SubjectRef ss = doc_.connection_subject(a.target);
        if (a.part == AnchorPart::Hole) {
          if (a.hole >= cb->hole_centres.size()) {
            f = {"draw.dimension.unresolved",
                 fmt::format("it has {} bolt holes, not {}", cb->hole_centres.size(), a.hole + 1),
                 ss};
            return std::nullopt;
          }
          const dvec3 c = cb->hole_centres[a.hole];
          return Located{dot(m, c), c};
        }
        if (a.part == AnchorPart::Centre) {
          if (cb->cylinder) {
            if (!parallel(cb->frame.normal(), frame.toward)) {
              f = {"draw.dimension.not_parallel", "its axis doesn't face the viewer", ss};
              return std::nullopt;
            }
            return Located{dot(m, cb->frame.origin), cb->frame.origin};
          }
          const auto box = kernel::bounding_box(cb->patch);
          return Located{dot(m, box.center()), box.center()};
        }
        const bool max = a.part == AnchorPart::Max;
        const auto pts = shape_points(cb->patch, model_deflection(), options_.settings);
        const auto at = extreme(pts, m, max, prefer);
        const auto e = kernel::extent(cb->patch, m);
        if (!at || !e) {
          f = {"draw.dimension.unresolved", "its patch has no extent", ss};
          return std::nullopt;
        }
        return Located{max ? e->second : e->first, *at};
      }
    }
    return std::nullopt;
  }

  std::optional<Circle> circle(const Anchor& a, const ViewFrame& frame, Failure& f) {
    if (a.on == AnchorOn::Surface) {
      const auto* cb = surface_build(a, f);
      if (!cb) return std::nullopt;
      const SubjectRef ss = doc_.connection_subject(a.target);
      if (!parallel(cb->frame.normal(), frame.toward)) {
        f = {"draw.dimension.not_circle", "it doesn't face the viewer", ss};
        return std::nullopt;
      }
      if (a.part == AnchorPart::Hole) {
        const auto* c = doc_.find_connection(a.target);
        const auto* bolted = c ? std::get_if<model::BoltedFlangeParams>(&c->params) : nullptr;
        const auto size = bolted ? model::thread_size(bolted->thread) : std::nullopt;
        if (!size || a.hole >= cb->hole_centres.size()) {
          f = {"draw.dimension.unresolved", "it has no such bolt hole", ss};
          return std::nullopt;
        }
        return Circle{cb->hole_centres[a.hole], size->clearance_hole};
      }
      if (a.part == AnchorPart::Centre && cb->cylinder) {
        return Circle{cb->frame.origin, 2 * cb->cylinder->radius};
      }
      f = {"draw.dimension.not_circle", "a planar patch has no diameter", ss};
      return std::nullopt;
    }
    if ((a.on == AnchorOn::VolumeBoundary || a.on == AnchorOn::VolumeContents) &&
        a.part == AnchorPart::Face) {
      const auto found = find_face(a, f);
      if (!found) return std::nullopt;
      const auto& nf = *found->face;
      const SubjectRef vs = doc_.volume_subject(a.target);
      if (!nf.cylinder) {
        f = {"draw.dimension.not_circle", fmt::format("face '{}' isn't a cylinder", a.face), vs};
        return std::nullopt;
      }
      if (!parallel(nf.cylinder->axis.direction, frame.toward)) {
        f = {"draw.dimension.not_circle",
             fmt::format("face '{}' is a cylinder whose axis doesn't face the viewer", a.face), vs};
        return std::nullopt;
      }
      return Circle{nf.cylinder->axis.origin, 2 * nf.cylinder->radius};
    }
    f = {"draw.dimension.not_circle", "it has no diameter", SubjectRef{}};
    return std::nullopt;
  }

  // --- dimensions -------------------------------------------------------------------

  void fail(const model::DrawingDimension& d, DimensionResult& r, const Anchor& a,
            const Failure& f) {
    const SubjectRef self = item_subject(d.id);
    std::vector<SubjectRef> subjects{self};
    if (!f.subject.label.empty()) subjects.push_back(f.subject);
    if (a.disposable() || f.code == "draw.dimension.lost") {
      out_.diagnostics.add(Diagnostic::warning(
          "draw.dimension.lost",
          fmt::format("'{}' is on generated contents, which changed: {}; it isn't drawn",
                      self.label, f.why),
          std::move(subjects)));
    } else {
      out_.diagnostics.add(
          Diagnostic::error(f.code, fmt::format("'{}': {}", self.label, f.why), std::move(subjects)));
    }
    (void)r;
  }

  void arrow(Pen pen, const Id& source, dvec2 tip, dvec2 dir) {
    const dvec2 n{-dir.y, dir.x};
    const dvec2 base = tip - dir * kArrow;
    path(pen, source, {tip, base + n * kArrowHalf, base - n * kArrowHalf}, true, true);
  }

  void dimension(std::size_t index) {
    const auto& d = drawing_.dimensions[index];
    DimensionResult r;
    r.dimension = d.id;
    r.disposable = d.disposable();
    const auto placed = placed_.find(d.view);
    if (placed == placed_.end()) {
      const SubjectRef self = item_subject(d.id);
      out_.diagnostics.add(Diagnostic::error(
          "draw.dimension.unresolved", fmt::format("'{}': its view isn't on the drawing", self.label),
          {self}));
      out_.dimensions.push_back(std::move(r));
      return;
    }
    const Placed& p = placed->second;
    const Pen pen = r.disposable ? Pen::Disposable : Pen::Dimension;
    const double th = options_.text_height;
    auto label = [&](std::string s) { return r.disposable ? "(" + s + ")" : s; };

    if (d.kind == DimensionKind::Diameter) {
      Failure f;
      const auto c = circle(d.from, p.frame, f);
      if (!c) {
        fail(d, r, d.from, f);
        out_.dimensions.push_back(std::move(r));
        return;
      }
      r.value = c->diameter;
      r.text = label("⌀" + number_text(c->diameter, drawing_.decimals));
      const dvec2 centre = to_sheet(d.view, p.frame.project(c->centre));
      const double radius = c->diameter / 2 * p.scale;
      const dvec2 dir{std::sqrt(0.5), std::sqrt(0.5)};
      const dvec2 on = centre + dir * radius;
      const double reach = std::max(std::abs(d.offset), kArrow + 2);
      const dvec2 elbow = on + dir * reach;
      const double tw = text_width(r.text, th);
      arrow(pen, d.id, on, dvec2{-dir.x, -dir.y});
      path(pen, d.id, {on, elbow, elbow + dvec2{tw + 2, 0}});
      text(pen, d.id, elbow + dvec2{1, 1}, th, r.text);
      r.drawn = true;
      out_.dimensions.push_back(std::move(r));
      return;
    }

    const bool horizontal = d.kind == DimensionKind::Horizontal;
    const dvec3 m = horizontal ? p.frame.right : p.frame.up;
    const dvec3 across = horizontal ? p.frame.up : p.frame.right;
    const dvec3 prefer = d.offset >= 0 ? across : -across;
    Failure f;
    const auto a = locate(d.from, m, prefer, p.frame, f);
    if (!a) {
      fail(d, r, d.from, f);
      out_.dimensions.push_back(std::move(r));
      return;
    }
    const auto b = locate(d.to, m, prefer, p.frame, f);
    if (!b) {
      fail(d, r, d.to, f);
      out_.dimensions.push_back(std::move(r));
      return;
    }
    const double value = std::abs(b->along - a->along);
    r.value = value;
    r.text = label(number_text(value, drawing_.decimals));

    // In sheet coordinates, u along the dimension, v across it.
    auto sheet_uv = [&](const Located& l) {
      const dvec2 at = to_sheet(d.view, p.frame.project(l.at));
      const dvec2 c = p.centre_sheet;
      const dvec2 cm = p.centre_model;
      const double u = (horizontal ? c.x - cm.x * p.scale : c.y - cm.y * p.scale) + l.along * p.scale;
      return dvec2{u, horizontal ? at.y : at.x};
    };
    auto to_xy = [&](double u, double v) { return horizontal ? dvec2{u, v} : dvec2{v, u}; };
    const dvec2 pa = sheet_uv(*a);
    const dvec2 pb = sheet_uv(*b);
    const double line = d.offset >= 0 ? std::max(pa.y, pb.y) + d.offset
                                      : std::min(pa.y, pb.y) + d.offset;
    for (const dvec2& q : {pa, pb}) {
      const double s = line >= q.y ? 1.0 : -1.0;
      if (std::abs(line - q.y) <= kGap) continue;
      path(pen, d.id, {to_xy(q.x, q.y + s * kGap), to_xy(q.x, line + s * kOvershoot)});
    }
    const double u0 = std::min(pa.x, pb.x);
    const double u1 = std::max(pa.x, pb.x);
    const dvec2 du = to_xy(1, 0) - to_xy(0, 0);
    if (u1 - u0 >= 2 * kArrow + 1) {
      path(pen, d.id, {to_xy(u0, line), to_xy(u1, line)});
      arrow(pen, d.id, to_xy(u0, line), -du);
      arrow(pen, d.id, to_xy(u1, line), du);
    } else {
      // Too short: arrows outside, pointing in.
      path(pen, d.id, {to_xy(u0 - kArrow - 2, line), to_xy(u1 + kArrow + 2, line)});
      arrow(pen, d.id, to_xy(u0, line), du);
      arrow(pen, d.id, to_xy(u1, line), -du);
    }
    const double mid = (u0 + u1) / 2;
    if (horizontal) {
      text(pen, d.id, {mid, line + 1}, th, r.text, TextAlign::Centre);
    } else {
      text(pen, d.id, {line - 1, mid}, th, r.text, TextAlign::Centre, true);
    }
    r.drawn = true;
    out_.dimensions.push_back(std::move(r));
  }
};

}  // namespace

DrawingResult draw(const eval::Evaluation& evaluation, const model::Drawing& drawing,
                   const DrawOptions& options) {
  return Drafter(evaluation, drawing, options).run();
}

DrawingResult draw(const eval::Evaluation& evaluation, const Id& drawing,
                   const DrawOptions& options) {
  if (const auto* d = evaluation.document.find_drawing(drawing)) return draw(evaluation, *d, options);
  DrawingResult out;
  out.diagnostics.add(Diagnostic::error(
      "draw.drawing.missing", fmt::format("the document has no drawing {}", drawing.to_string()),
      {{SubjectKind::Drawing, drawing, "(missing drawing)"}}));
  return out;
}

DrawingResult draw(const eval::Evaluation& evaluation, std::string_view drawing,
                   const DrawOptions& options) {
  if (const auto* d = evaluation.document.find_drawing(drawing)) return draw(evaluation, *d, options);
  DrawingResult out;
  out.diagnostics.add(Diagnostic::error(
      "draw.drawing.missing", fmt::format("the document has no drawing named '{}'", drawing),
      {{SubjectKind::Drawing, {}, std::string(drawing)}}));
  return out;
}

model::Drawing standard_drawing(const model::Document& document,
                                const model::DocumentBuild& layout, std::string name, double width,
                                double height) {
  model::Drawing d;
  d.id = Id::random();
  d.name = std::move(name);
  d.width = width;
  d.height = height;

  // The root volumes' boxes.
  struct Box {
    Id volume;
    core::dbox3 box;
  };
  std::vector<Box> boxes;
  core::dbox3 all;
  for (const auto& v : document.volumes) {
    if (!v.parent.is_null() || v.generated) continue;
    const auto* vb = layout.find_volume(v.id);
    if (!vb || vb->boundary.is_empty()) continue;
    const auto box = kernel::bounding_box(vb->boundary);
    if (box.is_empty()) continue;
    boxes.push_back({v.id, box});
    all.expand(box);
  }
  const dvec3 size = all.is_empty() ? dvec3{100, 100, 100} : all.size();
  // The isometric view's extent: the box's corners, projected.
  const ViewFrame iso = view_frame(model::ViewDirection::Isometric);
  dvec2 ilo{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
  dvec2 ihi = -ilo;
  for (int k = 0; k < 8; ++k) {
    const dvec3 c{k & 1 ? size.x : 0, k & 2 ? size.y : 0, k & 4 ? size.z : 0};
    const dvec2 q = iso.project(c);
    ilo = core::min(ilo, q);
    ihi = core::max(ihi, q);
  }
  const dvec2 isize = ihi - ilo;

  const double margin = std::min({10.0, width / 20, height / 20});
  const double title = 4 * std::min(8.0, (height - 2 * margin) / 8);
  const double gap = std::clamp(std::min(width, height) * 0.085, 12.0, 25.0);
  const double avail_w = width - 2 * margin - 3 * gap;
  const double avail_h = height - 2 * margin - title - 3 * gap;
  const double col2 = std::max(size.y, isize.x);
  const double row2 = std::max(size.y, isize.y);
  const double fit = std::min(avail_w / (size.x + col2), avail_h / (size.z + row2));
  const double s = standard_scale(std::max(fit, 0.0));

  // The 2 x 2 block, centred in the space above the title block.
  const double block_w = s * (size.x + col2) + gap;
  const double block_h = s * (size.z + row2) + gap;
  const double left = margin + (width - 2 * margin - block_w) / 2;
  const double bottom = margin + title + (height - 2 * margin - title - block_h) / 2;
  const double x1 = left + s * size.x / 2;
  const double x2 = left + s * size.x + gap + s * col2 / 2;
  const double y1 = bottom + s * size.z / 2;
  const double y2 = bottom + s * size.z + gap + s * row2 / 2;
  auto view = [&](std::string vname, model::ViewDirection dir, double x, double y) {
    model::DrawingView v;
    v.id = Id::random();
    v.name = std::move(vname);
    v.direction = dir;
    v.x = x;
    v.y = y;
    v.scale = s;
    d.views.push_back(v);
    return v.id;
  };
  const Id front = view("Front", model::ViewDirection::Front, x1, y1);
  const Id top = view("Top", model::ViewDirection::Top, x1, y2);
  view("Right", model::ViewDirection::Right, x2, y1);
  view("Isometric", model::ViewDirection::Isometric, x2, y2);

  if (boxes.empty()) return d;
  auto at = [&](int axis, bool max) {
    const Box* best = &boxes.front();
    for (const auto& b : boxes) {
      const double v = max ? b.box.max[static_cast<std::size_t>(axis)]
                           : b.box.min[static_cast<std::size_t>(axis)];
      const double w = max ? best->box.max[static_cast<std::size_t>(axis)]
                           : best->box.min[static_cast<std::size_t>(axis)];
      if (max ? v > w : v < w) best = &b;
    }
    return model::Anchor{AnchorOn::VolumeBoundary, best->volume,
                         max ? AnchorPart::Max : AnchorPart::Min, {}, 0};
  };
  d.dimensions.push_back({Id::random(), front, DimensionKind::Horizontal, at(0, false), at(0, true),
                          -10.0});
  d.dimensions.push_back({Id::random(), front, DimensionKind::Vertical, at(2, false), at(2, true),
                          -10.0});
  d.dimensions.push_back({Id::random(), top, DimensionKind::Vertical, at(1, false), at(1, true),
                          -10.0});
  return d;
}

}  // namespace vs::draw

#include "vs/validate/layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "vs/kernel/operations.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/kernel/tessellate.hpp"
#include "vs/model/hierarchy.hpp"
#include "vs/model/index.hpp"
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
};

// A volume as one chunk of pairs sees it: its boundary (a deep copy when
// chunks run on several threads, since OCCT's distance tools may fill
// caches on the shapes they read) and, once needed, its faces.
struct Side {
  const Prepared* p = nullptr;
  kernel::Solid boundary;
  std::vector<BoundaryFace> faces;
  bool faces_ready = false;
};

// What checking one pair found, folded into the report in pair order.
struct PairOutcome {
  bool tested = false;
  Diagnostics found;     // overlaps and undecided pairs
  Diagnostics contacts;  // contact warnings, reported after every pair's `found`
  std::vector<Overlap> overlaps;
  std::vector<Contact> touching;
};

// Pairs per chunk: enough to reuse each chunk's copies of its boundaries.
constexpr std::size_t kPairsPerChunk = 32;

Diagnostics connection_rules(const model::DocumentIndex& index, const DocumentBuild& build,
                             const ProductSettings& settings, const ParallelFor& parallel);

class LayoutChecker {
 public:
  LayoutChecker(const Document& doc, const model::DocumentIndex& index, const DocumentBuild& build,
                const ProductSettings& settings, const ParallelFor& parallel)
      : doc_(doc), ix_(index), build_(build), settings_(settings), parallel_(parallel),
        tol_(settings.linear_tolerance_mm) {
    // Pairs a bonded or bolted surface joins (shares_surface()).
    for (const auto& c : doc_.connections) {
      if (c.type() == model::ConnectionType::Clearance) continue;
      for (const Id& a : c.volumes) {
        for (const Id& b : c.volumes) bonded_.emplace(a, b);
      }
    }
  }

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

    // Only siblings are checked against each other (L3): a child is inside
    // its parent, and meets its parent's neighbours only through surfaces
    // delegated to it.
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    for (const auto& [i, j] : candidate_pairs(vols)) {
      if (vols[i].volume->parent == vols[j].volume->parent) pairs.emplace_back(i, j);
    }
    std::vector<PairOutcome> outcomes(pairs.size());
    const std::size_t chunks = (pairs.size() + kPairsPerChunk - 1) / kPairsPerChunk;
    const bool threads = static_cast<bool>(parallel_) && chunks > 1;
    const auto chunk = [&](std::size_t k) {
      std::unordered_map<std::size_t, Side> sides;  // this chunk's view of each volume
      const auto side = [&](std::size_t i) -> Side* {
        auto it = sides.find(i);
        if (it != sides.end()) return &it->second;
        Side s;
        s.p = &vols[i];
        if (threads) {
          auto copy = kernel::deep_copy(vols[i].build->boundary);
          if (!copy) return nullptr;
          s.boundary = std::move(*copy);
        } else {
          s.boundary = vols[i].build->boundary;
        }
        return &sides.emplace(i, std::move(s)).first->second;
      };
      const std::size_t end = std::min(pairs.size(), (k + 1) * kPairsPerChunk);
      for (std::size_t n = k * kPairsPerChunk; n < end; ++n) {
        PairOutcome& out = outcomes[n];
        Side* a = side(pairs[n].first);
        Side* b = side(pairs[n].second);
        out.tested = true;
        if (a == nullptr || b == nullptr) {
          undecided(vols[pairs[n].first], vols[pairs[n].second], "overlap",
                    Diagnostic::error("kernel.copy.failed", "copying a boundary failed"), out);
          continue;
        }
        check_pair(*a, *b, out);
      }
    };
    if (threads) {
      parallel_(chunks, chunk);
    } else {
      for (std::size_t k = 0; k < chunks; ++k) chunk(k);
    }
    Diagnostics contacts;
    for (auto& out : outcomes) {
      if (out.tested) ++report_.pairs_tested;
      report_.diagnostics.append(out.found);
      contacts.append(out.contacts);
      for (auto& o : out.overlaps) report_.overlaps.push_back(std::move(o));
      for (auto& c : out.touching) report_.contacts.push_back(std::move(c));
    }
    report_.diagnostics.append(contacts);
    std::unordered_map<Id, std::size_t> first;  // the first prepared volume with an id
    for (std::size_t k = 0; k < vols.size(); ++k) first.emplace(vols[k].volume->id, k);
    for (const Prepared& child : vols) {
      if (!child.volume->parent.is_null()) nesting(child, vols, first);
    }
    report_.diagnostics.append(connection_rules(ix_, build_, settings_, parallel_));
    return std::move(report_);
  }

 private:
  const Document& doc_;
  const model::DocumentIndex& ix_;
  const DocumentBuild& build_;
  const ProductSettings& settings_;
  const ParallelFor& parallel_;
  double tol_;
  LayoutReport report_;
  // Feature labels of each level's layout, for face names (readable()).
  mutable std::mutex labels_mutex_;
  mutable std::map<Id, std::unordered_map<Id, std::string>> labels_;
  std::map<Id, bool> fills_box_;
  std::set<std::pair<Id, Id>> bonded_;  // both ways round

  // The pairs (i < j, in that order) whose boxes meet within the
  // tolerance: what testing every pair finds, by a sweep along x.
  std::vector<std::pair<std::size_t, std::size_t>> candidate_pairs(
      const std::vector<Prepared>& vols) const {
    std::vector<std::size_t> by_x(vols.size());
    for (std::size_t k = 0; k < by_x.size(); ++k) by_x[k] = k;
    std::sort(by_x.begin(), by_x.end(), [&](std::size_t a, std::size_t b) {
      return vols[a].box.min.x < vols[b].box.min.x || (vols[a].box.min.x == vols[b].box.min.x && a < b);
    });
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (std::size_t p = 0; p < by_x.size(); ++p) {
      const Prepared& a = vols[by_x[p]];
      for (std::size_t q = p + 1; q < by_x.size(); ++q) {
        const Prepared& b = vols[by_x[q]];
        // Sorted by min.x: once b starts past a's end, so do the rest.
        if (b.box.min.x > a.box.max.x + tol_) break;
        if (!a.box.intersects(b.box, tol_)) continue;
        out.emplace_back(std::min(by_x[p], by_x[q]), std::max(by_x[p], by_x[q]));
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  // A bonded or bolted surface between them: they meet there by design. A
  // clearance surface keeps them apart by its gap, so any contact between
  // them is still reported.
  bool shares_surface(const Id& a, const Id& b) const { return bonded_.contains({a, b}); }

  void check_pair(Side& a, Side& b, PairOutcome& out) const {
    if (overlap(a, b, out)) return;
    if (shares_surface(a.p->volume->id, b.p->volume->id)) return;  // adjacent by definition
    if (a.p->volume->keep_out || b.p->volume->keep_out) return;
    contact(a, b, out);
  }

  void undecided(const Prepared& a, const Prepared& b, std::string_view what,
                 const Diagnostic& why, PairOutcome& out) const {
    out.found.add(Diagnostic::error(
        "validate.undecided",
        fmt::format("Couldn't check '{}' and '{}' for {}: {}", a.volume->name, b.volume->name,
                    what, why.message),
        {ix_.boundary_subject(a.volume->id), ix_.boundary_subject(b.volume->id)}));
  }

  // True when the pair overlaps (reported) or couldn't be checked.
  bool overlap(const Side& sa, const Side& sb, PairOutcome& out) const {
    const Prepared& a = *sa.p;
    const Prepared& b = *sb.p;
    // The overlap lies inside both boxes, so it is no deeper than their
    // intersection's smallest side: neighbours that only touch skip the
    // boolean.
    const dbox3 meet = core::intersection(a.box, b.box);
    if (meet.is_empty() || smallest_side(meet) <= tol_) return false;

    auto common = kernel::common(sa.boundary, sb.boundary, {tol_});
    if (!common) {
      undecided(a, b, "overlap", common.error(), out);
      return true;
    }
    if (common->is_empty()) return false;
    const dbox3 region = kernel::bounding_box(*common);
    const double depth = smallest_side(region);
    const auto mass = kernel::mass_properties(*common);
    const double volume = mass ? mass->volume : 0.0;
    if (depth <= tol_ || volume <= 0.0) return false;  // touching

    out.overlaps.push_back({a.volume->id, b.volume->id, region, volume, depth});
    out.found.add(Diagnostic::error(
        "validate.overlap",
        fmt::format("'{}' and '{}' overlap by {} mm ({} mm³ from {}); move or resize one, "
                    "or cut one with a keep-out region",
                    a.volume->name, b.volume->name, mm(depth), mm(volume), box_text(region)),
        {ix_.boundary_subject(a.volume->id), ix_.boundary_subject(b.volume->id)}));
    return true;
  }

  // --- nesting (L3) ----------------------------------------------------------------

  // A child's boundary must lie inside its composite's: whatever pokes out
  // is reported with its depth (the region's thinnest side), as overlap is.
  void nesting(const Prepared& child, const std::vector<Prepared>& vols,
               const std::unordered_map<Id, std::size_t>& first) {
    const auto found = first.find(child.volume->parent);
    if (found == first.end()) return;  // the parent's boundary failed: reported already
    const auto parent = vols.begin() + static_cast<std::ptrdiff_t>(found->second);
    // Inside the box of a parent that is exactly its box: nothing to check.
    // Anything else (a notch, a curved face) needs the boolean.
    if (parent->box.contains(child.box, tol_) && fills_box(*parent)) return;
    auto outside = kernel::cut(child.build->boundary, std::vector<Solid>{parent->build->boundary}, {tol_});
    if (!outside) {
      if (outside.error().code == "kernel.boolean.empty_result") return;
      PairOutcome why;
      undecided(child, *parent, "nesting", outside.error(), why);
      report_.diagnostics.append(why.found);
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
        {ix_.boundary_subject(child.volume->id), ix_.boundary_subject(parent->volume->id)}));
  }

  // True when the boundary is exactly its bounding box: one solid with six
  // planar faces, each facing out of a different side of the box and
  // spanning all of it. Decided once per volume.
  bool fills_box(const Prepared& p) {
    const auto known = fills_box_.find(p.volume->id);
    if (known != fills_box_.end()) return known->second;
    const auto faces = kernel::faces(p.build->boundary);
    bool fills = p.build->boundary.solid_count() == 1 && faces.size() == 6;
    std::array<bool, 6> taken{};  // -x, +x, -y, +y, -z, +z
    for (std::size_t i = 0; fills && i < faces.size(); ++i) {
      const auto plane = kernel::face_plane(faces[i]);
      fills = plane.has_value() && side_of(p.box, kernel::bounding_box(faces[i]),
                                           normalize(plane->normal), taken);
    }
    fills_box_.emplace(p.volume->id, fills);
    return fills;
  }

  // Marks the side of `box` that a face (its box and outward normal) covers
  // exactly, if any and not marked already.
  bool side_of(const dbox3& box, const dbox3& face, const dvec3& normal,
               std::array<bool, 6>& taken) const {
    const auto at = [](const dvec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; };
    for (int axis = 0; axis < 3; ++axis) {
      const double along = at(normal, axis);
      if (std::abs(along) < 1.0 - 1e-12) continue;
      const bool high = along > 0.0;
      const double plane = high ? at(box.max, axis) : at(box.min, axis);
      // Flat on that side, and as large as it in the other two directions.
      if (std::abs(at(face.min, axis) - plane) > tol_ || std::abs(at(face.max, axis) - plane) > tol_) {
        return false;
      }
      for (int other = 0; other < 3; ++other) {
        if (other == axis) continue;
        if (std::abs(at(face.min, other) - at(box.min, other)) > tol_ ||
            std::abs(at(face.max, other) - at(box.max, other)) > tol_) {
          return false;
        }
      }
      auto& slot = taken[static_cast<std::size_t>(axis * 2 + (high ? 1 : 0))];
      if (slot) return false;
      slot = true;
      return true;
    }
    return false;  // not axis-aligned
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
    const feature::FeatureTree* layout = ix_.level_layout(level);
    return fmt::format("{}:{}", layout != nullptr ? label(*layout, level, *id) : id->to_string(),
                       name.substr(colon + 1));
  }

  // layout.label(id), from a table made once per level rather than a
  // search through the layout each time.
  std::string label(const feature::FeatureTree& layout, const Id& level, const Id& id) const {
    const std::lock_guard lock(labels_mutex_);
    auto it = labels_.find(level);
    if (it == labels_.end()) {
      std::unordered_map<Id, std::string> table;
      for (const auto& s : layout.subjects()) table.emplace(s.id, s.label);
      it = labels_.emplace(level, std::move(table)).first;
    }
    const auto found = it->second.find(id);
    return found != it->second.end() ? found->second : layout.label(id);
  }

  const std::vector<BoundaryFace>& faces_of(Side& s) const {
    if (!s.faces_ready) {
      const auto list = kernel::faces(s.boundary);
      for (std::size_t k = 0; k < list.size(); ++k) {
        s.faces.push_back({list[k], kernel::bounding_box(list[k]), kernel::face_plane(list[k]),
                           face_label(*s.p, list[k], k)});
      }
      s.faces_ready = true;
    }
    return s.faces;
  }

  SubjectRef face_subject(const Prepared& p, const std::string& face) const {
    SubjectRef s = ix_.boundary_subject(p.volume->id);
    s.label = fmt::format("{}, face '{}'", s.label, face);
    return s;
  }

  void contact(Side& sa, Side& sb, PairOutcome& outcome) const {
    const Prepared& a = *sa.p;
    const Prepared& b = *sb.p;
    Diagnostics& out = outcome.contacts;
    auto dist = kernel::min_distance(sa.boundary, sb.boundary);
    if (!dist) {
      undecided(a, b, "contact", dist.error(), outcome);
      return;
    }
    if (dist->distance > tol_) return;

    const double min_area = settings_.min_feature_size_mm() * settings_.min_feature_size_mm();
    bool any = false;
    for (const auto& fa : faces_of(sa)) {
      for (const auto& fb : faces_of(sb)) {
        if (!fa.box.intersects(fb.box, tol_)) continue;
        if (fa.plane && fb.plane) {
          // Planar faces lie against each other only when they face each
          // other on one plane.
          if (dot(fa.plane->normal, fb.plane->normal) > -1.0 + 1e-9) continue;
          if (std::abs(dot(fb.plane->origin - fa.plane->origin, fa.plane->normal)) > tol_) continue;
        }
        auto shared = kernel::common_faces(fa.face, fb.face, {tol_});
        if (!shared) {
          undecided(a, b, "contact", shared.error(), outcome);
          return;
        }
        if (shared->is_empty()) continue;
        const double area = kernel::surface_area(*shared);
        if (area <= min_area) continue;
        any = true;
        const dbox3 region = kernel::bounding_box(*shared);
        outcome.touching.push_back({a.volume->id, b.volume->id, fa.name, fb.name, area, region});
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
    outcome.touching.push_back({a.volume->id, b.volume->id, {}, {}, 0.0, region});
    out.add(Diagnostic::warning(
        "validate.contact.edge",
        fmt::format("'{}' and '{}' touch along an edge or at a point near ({}, {}, {}), with no "
                    "connection surface between them; move them apart or let them meet face "
                    "to face through a connection surface",
                    a.volume->name, b.volume->name, mm(dist->point_on_a.x),
                    mm(dist->point_on_a.y), mm(dist->point_on_a.z)),
        {ix_.boundary_subject(a.volume->id), ix_.boundary_subject(b.volume->id)}));
  }
};

// --- connection rules ------------------------------------------------------------

double box_distance(const dbox3& b, const dvec3& p) {
  const dvec3 d{std::max({b.min.x - p.x, 0.0, p.x - b.max.x}),
                std::max({b.min.y - p.y, 0.0, p.y - b.max.y}),
                std::max({b.min.z - p.z, 0.0, p.z - b.max.z})};
  return length(d);
}

// Points spread over a patch: its vertices, its edges' and faces' mesh
// points and its triangles' centres (coarse: a patch's outline and a few
// interior points).
core::Result<std::vector<dvec3>> patch_points(const Face& patch, const ProductSettings& settings) {
  std::vector<dvec3> points = kernel::vertex_points(patch);
  const double diagonal = length(kernel::bounding_box(patch).size());
  kernel::TessellationParams params;
  params.deflection = std::max(settings.min_feature_size_mm(), diagonal * 0.02);
  params.angle = 0.5;
  auto tess = kernel::tessellate(patch, params, settings);
  if (!tess) return tess.error();
  for (const auto& e : tess->edges) points.insert(points.end(), e.points.begin(), e.points.end());
  for (const auto& f : tess->faces) {
    points.insert(points.end(), f.points.begin(), f.points.end());
    // A patch's mesh may be its outline alone (a rectangle is two
    // triangles): each triangle's centre, taken in (u, v) and put back on
    // the surface (a curved patch's chords are off it), reaches inside.
    for (const auto& t : f.triangles) {
      const core::dvec2 uv = (f.uv[t[0]] + f.uv[t[1]] + f.uv[t[2]]) / 3.0;
      const dvec3 p = kernel::face_point(f.face, uv);
      if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) points.push_back(p);
    }
  }
  return points;
}

// How deep the deepest of `points` lies inside `solid`: its distance to the
// solid's surface (its faces, not its inside), 0 when every point is on or
// outside it. Points outside are left to the patch's distance to the solid,
// as before: a patch may run past the face it lies on.
core::Result<double> deepest_inside(const std::vector<dvec3>& points, const Solid& solid,
                                    const ProductSettings& settings, double reach) {
  const kernel::SolidClassifier classifier(solid, settings);
  const auto faces = kernel::faces(solid);
  std::vector<dbox3> boxes;
  boxes.reserve(faces.size());
  for (const auto& f : faces) boxes.push_back(kernel::bounding_box(f));
  double worst = 0.0;
  for (const dvec3& p : points) {
    if (classifier.classify(p) != kernel::PointClass::Inside) continue;
    double best = std::numeric_limits<double>::infinity();
    // Faces whose box is nearest first: usually one of them is the answer.
    std::vector<std::size_t> order(faces.size());
    for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      return box_distance(boxes[a], p) < box_distance(boxes[b], p);
    });
    for (const std::size_t k : order) {
      if (box_distance(boxes[k], p) >= best) break;  // no closer face is left
      auto d = kernel::distance_to(faces[k], p);
      if (!d) return d.error();
      best = std::min(best, *d);
      if (best <= reach) break;
    }
    worst = std::max(worst, best);
  }
  return worst;
}

std::string params_text(std::initializer_list<std::string_view> names) {
  std::string out;
  for (auto n : names) {
    if (!out.empty()) out += ", ";
    out += n;
  }
  return out;
}

void check_bolts(const SubjectRef& self, const model::ConnectionSurface& c,
                 const model::BoltedFlangeParams& params, const ConnectionBuild& cb,
                 const ProductSettings& settings, Diagnostics& out) {
  const double tol = settings.linear_tolerance_mm;
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

namespace {

// Surfaces per chunk of the connection rules.
constexpr std::size_t kSurfacesPerChunk = 16;

Diagnostics connection_rules(const model::DocumentIndex& index, const DocumentBuild& build,
                             const ProductSettings& settings, const ParallelFor& parallel) {
  const Document& document = index.document();
  // The build's surfaces and volumes by id, the first of each as its
  // find_connection() and find_volume() find them.
  std::unordered_map<Id, const ConnectionBuild*> built_surfaces;
  for (const auto& cb : build.connections) built_surfaces.emplace(cb.surface, &cb);
  std::unordered_map<Id, const VolumeBuild*> built_volumes;
  for (const auto& vb : build.volumes) built_volumes.emplace(vb.volume, &vb);
  const double tol = settings.linear_tolerance_mm;
  const std::size_t count = document.connections.size();
  std::vector<Diagnostics> found_by_surface(count);
  const std::size_t chunks = (count + kSurfacesPerChunk - 1) / kSurfacesPerChunk;
  const bool threads = static_cast<bool>(parallel) && chunks > 1;
  // One surface's rules. On several threads each chunk reads its own deep
  // copies of the boundaries (`boundary_of`); a patch is read by its own
  // surface's task alone.
  const auto check_one = [&](const model::ConnectionSurface& c, Diagnostics& out,
                             const auto& boundary_of) {
    const auto found = built_surfaces.find(c.id);
    const ConnectionBuild* cb = found != built_surfaces.end() ? found->second : nullptr;
    if (!cb || !cb->ok) return;
    const SubjectRef self = index.connection_subject(c.id);

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
        at = index.delegate_of(at, c.id);
      }
    }
    // Points spread over the patch: none may lie inside a volume it joins
    // farther than the reach from its surface. The patch's distance to the
    // solid alone is zero for a patch floating inside it or poking through.
    std::optional<std::vector<dvec3>> samples;
    for (const auto& vid : met) {
      const auto built = built_volumes.find(vid);
      const VolumeBuild* vb = built != built_volumes.end() ? built->second : nullptr;
      if (!vb || vb->boundary.is_empty()) continue;
      auto undecided = [&](const std::string& why) {
        out.add(Diagnostic::error(
            "validate.undecided",
            fmt::format("Couldn't check that '{}' lies on '{}': {}", c.name,
                        index.volume_subject(vid).label, why),
            {self, index.boundary_subject(vid)}));
      };
      const kernel::Solid* boundary = boundary_of(*vb);
      if (boundary == nullptr) {
        undecided("copying its boundary failed");
        continue;
      }
      auto d = kernel::min_distance(cb->patch, *boundary);
      if (!d) {
        undecided(d.error().message);
        continue;
      }
      double off = d->distance;
      bool inside = false;
      if (off <= reach) {
        if (!samples) {
          auto s = patch_points(cb->patch, settings);
          if (!s) {
            undecided(s.error().message);
            continue;
          }
          samples = std::move(*s);
        }
        auto far = deepest_inside(*samples, *boundary, settings, reach);
        if (!far) {
          undecided(far.error().message);
          continue;
        }
        inside = *far > off;
        off = std::max(off, *far);
      }
      if (off > reach) {
        const bool clearance = c.type() == model::ConnectionType::Clearance;
        out.add(Diagnostic::error(
            "validate.connection.off_boundary",
            fmt::format("'{}' {} {} mm {} the boundary of '{}', which it joins; a {} surface "
                        "must lie on {} (parameters: geometry{})",
                        c.name, inside ? "reaches" : "is", mm(off),
                        inside ? "inside" : "from", index.volume_subject(vid).label,
                        to_string(c.type()),
                        clearance ? "it, or within the gap of it" : "it",
                        clearance ? ", gap" : ""),
            {self, index.boundary_subject(vid)}));
      }
    }

    if (const auto* bolts = std::get_if<model::BoltedFlangeParams>(&c.params)) {
      check_bolts(self, c, *bolts, *cb, settings, out);
    }
  };
  const auto chunk = [&](std::size_t k) {
    std::unordered_map<const VolumeBuild*, std::optional<kernel::Solid>> copies;
    const auto boundary_of = [&](const VolumeBuild& vb) -> const kernel::Solid* {
      if (!threads) return &vb.boundary;
      auto it = copies.find(&vb);
      if (it == copies.end()) {
        auto copy = kernel::deep_copy(vb.boundary);
        it = copies.emplace(&vb, copy ? std::optional<kernel::Solid>(std::move(*copy))
                                      : std::nullopt).first;
      }
      return it->second ? &*it->second : nullptr;
    };
    const std::size_t end = std::min(count, (k + 1) * kSurfacesPerChunk);
    for (std::size_t n = k * kSurfacesPerChunk; n < end; ++n) {
      check_one(document.connections[n], found_by_surface[n], boundary_of);
    }
  };
  if (threads) {
    parallel(chunks, chunk);
  } else {
    for (std::size_t k = 0; k < chunks; ++k) chunk(k);
  }
  Diagnostics out;
  for (const auto& found : found_by_surface) out.append(found);
  return out;
}

}  // namespace

Diagnostics check_connections(const Document& document, const DocumentBuild& build,
                              const ProductSettings& settings, const ParallelFor& parallel) {
  return connection_rules(model::DocumentIndex(document), build, settings, parallel);
}

LayoutReport validate_layout(const Document& document, const DocumentBuild& build,
                             const ProductSettings& settings, const ParallelFor& parallel) {
  const model::DocumentIndex index(document);
  return LayoutChecker(document, index, build, settings, parallel).run();
}

}  // namespace vs::validate

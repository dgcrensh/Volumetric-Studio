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

// Box sides, in the order side_of() marks them.
enum BoxSide : std::size_t { kMinX, kMaxX, kMinY, kMaxY, kMinZ, kMaxZ };

struct Prepared {
  const model::Volume* volume = nullptr;
  const VolumeBuild* build = nullptr;
  dbox3 box;
  // The boundary is exactly `box` (one solid, six axis-aligned planar faces
  // each spanning a side): pairs of boxes are checked by arithmetic
  // (decision 0032).
  bool is_box = false;
  // The index in kernel::faces() of the face on each side, by BoxSide.
  // Named only when a contact is reported (side_name()).
  std::array<std::size_t, 6> side_faces{};
};

// A volume as one chunk of pairs sees it: once a pair needs OCCT, its
// boundary (a deep copy when chunks run on several threads, since OCCT's
// distance tools may fill caches on the shapes they read; pairs of boxes
// never need one, decision 0034) and its faces.
struct Side {
  const Prepared* p = nullptr;
  std::optional<kernel::Solid> boundary;
  bool copied = false;  // `boundary` was made, or copying it failed
  std::vector<BoundaryFace> faces;
  bool faces_ready = false;
};

// A face contact the box path found, named on the calling thread.
struct BoxFaceContact {
  std::size_t a, b;            // prepared volumes
  std::size_t side_a, side_b;  // BoxSide
  double area;
  dbox3 region;
};

// What checking one pair found, folded into the report in pair order.
struct PairOutcome {
  bool tested = false;
  Diagnostics found;     // overlaps and undecided pairs
  Diagnostics contacts;  // contact warnings, reported after every pair's `found`
  std::vector<Overlap> overlaps;
  std::vector<Contact> touching;
  std::vector<BoxFaceContact> box_faces;  // named in the fold, before `contacts`
};

// Marks the side of `box` that a face (its box and outward normal) covers
// exactly, if any and not marked already.
bool side_of(const dbox3& box, const dbox3& face, const dvec3& normal, double tol,
           std::array<bool, 6>& taken) {
  const auto at = [](const dvec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; };
  for (int axis = 0; axis < 3; ++axis) {
    const double along = at(normal, axis);
    if (std::abs(along) < 1.0 - 1e-12) continue;
    const bool high = along > 0.0;
    const double plane = high ? at(box.max, axis) : at(box.min, axis);
    // Flat on that side, and as large as it in the other two directions.
    if (std::abs(at(face.min, axis) - plane) > tol || std::abs(at(face.max, axis) - plane) > tol) {
      return false;
    }
    for (int other = 0; other < 3; ++other) {
      if (other == axis) continue;
      if (std::abs(at(face.min, other) - at(box.min, other)) > tol ||
          std::abs(at(face.max, other) - at(box.max, other)) > tol) {
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

// When `solid` is exactly `box` (its bounding box): one solid with six
// planar faces, each facing out of a different side of the box and
// spanning all of it, the index in kernel::faces() of the face on each
// side (by BoxSide).
std::optional<std::array<std::size_t, 6>> box_sides(const Solid& solid, const dbox3& box,
                                                    double tol) {
  if (solid.solid_count() != 1) return std::nullopt;
  const auto faces = kernel::faces(solid);
  if (faces.size() != 6) return std::nullopt;
  std::array<bool, 6> taken{};
  std::array<std::size_t, 6> face_at{};
  for (std::size_t i = 0; i < faces.size(); ++i) {
    // Only the normal is read: no area integration (decision 0034).
    const auto plane = kernel::face_surface_plane(faces[i]);
    const std::array<bool, 6> before = taken;
    if (!plane ||
        !side_of(box, kernel::bounding_box(faces[i]), normalize(plane->normal), tol, taken)) {
      return std::nullopt;
    }
    for (std::size_t k = 0; k < 6; ++k) {
      if (taken[k] && !before[k]) face_at[k] = i;
    }
  }
  return face_at;
}

// The boundaries that are exactly their boxes, by build: the connection
// rules' box path (decision 0032).
using BoxBoundaries = std::unordered_map<const VolumeBuild*, dbox3>;

// Pairs per chunk: enough to reuse each chunk's copies of its boundaries.
constexpr std::size_t kPairsPerChunk = 32;
// Volumes per chunk when finding which boundaries are boxes.
constexpr std::size_t kVolumesPerChunk = 64;

Diagnostics connection_rules(const model::DocumentIndex& index, const DocumentBuild& build,
                             const ProductSettings& settings, const ParallelFor& parallel,
                             const BoxBoundaries& boxes, bool sample_every_patch);

class LayoutChecker {
 public:
  LayoutChecker(const Document& doc, const model::DocumentIndex& index, const DocumentBuild& build,
                const ProductSettings& settings, const ParallelFor& parallel,
                const LayoutOptions& options)
      : doc_(doc), ix_(index), build_(build), settings_(settings), parallel_(parallel),
        options_(options), tol_(settings.linear_tolerance_mm) {
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
      vols.push_back(std::move(p));
    }
    // Each volume's box and whether it is exactly its box, in fixed chunks
    // on every thread (decision 0034). A task reads only its own volumes'
    // boundaries, and only their geometry (bounding boxes, planes): no
    // classifier or distance tool, so nothing fills caches on them.
    {
      const std::size_t pchunks = (vols.size() + kVolumesPerChunk - 1) / kVolumesPerChunk;
      const auto prepare = [&](std::size_t k) {
        const std::size_t end = std::min(vols.size(), (k + 1) * kVolumesPerChunk);
        for (std::size_t n = k * kVolumesPerChunk; n < end; ++n) {
          vols[n].box = kernel::bounding_box(vols[n].build->boundary);
          classify_box(vols[n]);
        }
      };
      if (parallel_ && pchunks > 1) {
        parallel_(pchunks, prepare);
      } else {
        for (std::size_t k = 0; k < pchunks; ++k) prepare(k);
      }
    }

    vols_base_ = vols.data();
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
      const auto side = [&](std::size_t i) -> Side& {
        Side& s = sides[i];
        s.p = &vols[i];
        return s;
      };
      // The boundary a pair going through OCCT reads: a deep copy on
      // several threads, made the first time this chunk needs it.
      const auto solid = [&](Side& s) -> bool {
        if (!s.copied) {
          s.copied = true;
          if (threads) {
            auto copy = kernel::deep_copy(s.p->build->boundary);
            if (copy) s.boundary = std::move(*copy);
          } else {
            s.boundary = s.p->build->boundary;
          }
        }
        return s.boundary.has_value();
      };
      const std::size_t end = std::min(pairs.size(), (k + 1) * kPairsPerChunk);
      for (std::size_t n = k * kPairsPerChunk; n < end; ++n) {
        PairOutcome& out = outcomes[n];
        Side& a = side(pairs[n].first);
        Side& b = side(pairs[n].second);
        out.tested = true;
        const bool boxes = options_.box_fast_path && a.p->is_box && b.p->is_box;
        if (!boxes && !(solid(a) && solid(b))) {
          undecided(vols[pairs[n].first], vols[pairs[n].second], "overlap",
                    Diagnostic::error("kernel.copy.failed", "copying a boundary failed"), out);
          continue;
        }
        check_pair(a, b, out);
      }
    };
    if (threads) {
      parallel_(chunks, chunk);
    } else {
      for (std::size_t k = 0; k < chunks; ++k) chunk(k);
    }
    Diagnostics contacts;
    for (auto& out : outcomes) {
      for (const auto& c : out.box_faces) {
        report_contact(vols[c.a], vols[c.b], side_name(vols[c.a], c.side_a),
                       side_name(vols[c.b], c.side_b), c.area, c.region, out);
      }
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
    BoxBoundaries boxes;
    if (options_.box_fast_path) {
      for (const Prepared& p : vols) {
        if (p.is_box) boxes.emplace(p.build, p.box);
      }
    }
    report_.diagnostics.append(connection_rules(ix_, build_, settings_, parallel_, boxes,
                                                options_.sample_every_patch));
    return std::move(report_);
  }

 private:
  const Document& doc_;
  const model::DocumentIndex& ix_;
  const DocumentBuild& build_;
  const ProductSettings& settings_;
  const ParallelFor& parallel_;
  const LayoutOptions& options_;
  double tol_;
  const Prepared* vols_base_ = nullptr;
  LayoutReport report_;
  // Feature labels of each level's layout, for face names (readable()).
  mutable std::mutex labels_mutex_;
  mutable std::map<Id, std::unordered_map<Id, std::string>> labels_;
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
    const bool boxes = options_.box_fast_path && a.p->is_box && b.p->is_box;
    if (boxes ? box_overlap(*a.p, *b.p, out) : overlap(a, b, out)) return;
    if (shares_surface(a.p->volume->id, b.p->volume->id)) return;  // adjacent by definition
    if (a.p->volume->keep_out || b.p->volume->keep_out) return;
    if (boxes) {
      box_contact(*a.p, *b.p, out);
    } else {
      contact(a, b, out);
    }
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

    auto common = kernel::common(*sa.boundary, *sb.boundary, {tol_});
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

    report_overlap(a, b, region, volume, depth, out);
    return true;
  }

  void report_overlap(const Prepared& a, const Prepared& b, const dbox3& region, double volume,
                      double depth, PairOutcome& out) const {
    out.overlaps.push_back({a.volume->id, b.volume->id, region, volume, depth});
    out.found.add(Diagnostic::error(
        "validate.overlap",
        fmt::format("'{}' and '{}' overlap by {} mm ({} mm³ from {}); move or resize one, "
                    "or cut one with a keep-out region",
                    a.volume->name, b.volume->name, mm(depth), mm(volume), box_text(region)),
        {ix_.boundary_subject(a.volume->id), ix_.boundary_subject(b.volume->id)}));
  }

  // overlap() for two boxes: their common part is the boxes' intersection.
  bool box_overlap(const Prepared& a, const Prepared& b, PairOutcome& out) const {
    const dbox3 meet = core::intersection(a.box, b.box);
    if (meet.is_empty() || smallest_side(meet) <= tol_) return false;
    const dvec3 size = meet.size();
    report_overlap(a, b, meet, size.x * size.y * size.z, smallest_side(meet), out);
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
    if (parent->box.contains(child.box, tol_) && parent->is_box) return;
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

  // Whether the boundary is exactly its bounding box: one solid with six
  // planar faces, each facing out of a different side of the box and
  // spanning all of it; if so, the faces' names by side.
  void classify_box(Prepared& p) const {
    const auto sides = box_sides(p.build->boundary, p.box, tol_);
    if (!sides) return;
    p.is_box = true;
    p.side_faces = *sides;
  }

  // The name of a box's face on `side`, as faces_of() names it, so either
  // path reports the same names. On the calling thread, for the contacts
  // the box path found.
  std::string side_name(const Prepared& p, std::size_t side) const {
    const std::size_t index = p.side_faces[side];
    return face_label(p, kernel::faces(p.build->boundary)[index], index);
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
      const auto list = kernel::faces(*s.boundary);
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

  void report_contact(const Prepared& a, const Prepared& b, const std::string& face_a,
                      const std::string& face_b, double area, const dbox3& region,
                      PairOutcome& outcome) const {
    outcome.touching.push_back({a.volume->id, b.volume->id, face_a, face_b, area, region});
    outcome.contacts.add(Diagnostic::warning(
        "validate.contact",
        fmt::format("Face '{}' of '{}' touches face '{}' of '{}' over {} mm², with no "
                    "connection surface between them; add one, or move them apart",
                    face_a, a.volume->name, face_b, b.volume->name, mm(area)),
        {face_subject(a, face_a), face_subject(b, face_b)}));
  }

  void report_edge_contact(const Prepared& a, const Prepared& b, const dvec3& on_a,
                           const dvec3& on_b, PairOutcome& outcome) const {
    dbox3 region = dbox3::empty();
    region.expand(on_a);
    region.expand(on_b);
    outcome.touching.push_back({a.volume->id, b.volume->id, {}, {}, 0.0, region});
    outcome.contacts.add(Diagnostic::warning(
        "validate.contact.edge",
        fmt::format("'{}' and '{}' touch along an edge or at a point near ({}, {}, {}), with no "
                    "connection surface between them; move them apart or let them meet face "
                    "to face through a connection surface",
                    a.volume->name, b.volume->name, mm(on_a.x), mm(on_a.y), mm(on_a.z)),
        {ix_.boundary_subject(a.volume->id), ix_.boundary_subject(b.volume->id)}));
  }

  // contact() for two boxes that don't overlap. Faces lie against each
  // other where one box's side is within the tolerance of the other's
  // opposite side; what they share is the rectangle where both sides meet.
  // Otherwise boxes within the tolerance touch along an edge or at a corner,
  // reported at the middle of where they meet (OCCT's nearest points may be
  // anywhere along it).
  void box_contact(const Prepared& a, const Prepared& b, PairOutcome& outcome) const {
    const std::size_t ia = static_cast<std::size_t>(&a - vols_base_);
    const std::size_t ib = static_cast<std::size_t>(&b - vols_base_);
    const auto at = [](const dvec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; };
    double gap2 = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
      const double g = std::max({at(a.box.min, axis) - at(b.box.max, axis), 0.0,
                                 at(b.box.min, axis) - at(a.box.max, axis)});
      gap2 += g * g;
    }
    if (std::sqrt(gap2) > tol_) return;

    const double min_area = settings_.min_feature_size_mm() * settings_.min_feature_size_mm();
    bool any = false;
    for (int axis = 0; axis < 3; ++axis) {
      const int u = (axis + 1) % 3;
      const int v = (axis + 2) % 3;
      const double lo_u = std::max(at(a.box.min, u), at(b.box.min, u));
      const double hi_u = std::min(at(a.box.max, u), at(b.box.max, u));
      const double lo_v = std::max(at(a.box.min, v), at(b.box.min, v));
      const double hi_v = std::min(at(a.box.max, v), at(b.box.max, v));
      // A side no wider than the tolerance is an edge, as OCCT's fuzzy
      // common sees it.
      if (hi_u - lo_u <= tol_ || hi_v - lo_v <= tol_) continue;
      const double area = (hi_u - lo_u) * (hi_v - lo_v);
      if (area <= min_area) continue;
      // a's high side against b's low side, then a's low side against b's high side.
      for (const bool a_high : {true, false}) {
        const double plane_a = a_high ? at(a.box.max, axis) : at(a.box.min, axis);
        const double plane_b = a_high ? at(b.box.min, axis) : at(b.box.max, axis);
        if (std::abs(plane_a - plane_b) > tol_) continue;
        dvec3 lo, hi;
        const auto set = [](dvec3& p, int k, double value) {
          (k == 0 ? p.x : k == 1 ? p.y : p.z) = value;
        };
        set(lo, axis, plane_a);
        set(hi, axis, plane_a);
        set(lo, u, lo_u);
        set(hi, u, hi_u);
        set(lo, v, lo_v);
        set(hi, v, hi_v);
        const std::size_t side_a = static_cast<std::size_t>(axis * 2 + (a_high ? 1 : 0));
        const std::size_t side_b = static_cast<std::size_t>(axis * 2 + (a_high ? 0 : 1));
        outcome.box_faces.push_back({ia, ib, side_a, side_b, area, dbox3::from_min_max(lo, hi)});
        any = true;
      }
    }
    if (any) return;

    dvec3 on_a, on_b;
    for (int axis = 0; axis < 3; ++axis) {
      const double a_lo = at(a.box.min, axis), a_hi = at(a.box.max, axis);
      const double b_lo = at(b.box.min, axis), b_hi = at(b.box.max, axis);
      double pa = 0, pb = 0;
      if (a_hi < b_lo) {
        pa = a_hi;
        pb = b_lo;
      } else if (b_hi < a_lo) {
        pa = a_lo;
        pb = b_hi;
      } else {
        pa = pb = (std::max(a_lo, b_lo) + std::min(a_hi, b_hi)) / 2.0;
      }
      (axis == 0 ? on_a.x : axis == 1 ? on_a.y : on_a.z) = pa;
      (axis == 0 ? on_b.x : axis == 1 ? on_b.y : on_b.z) = pb;
    }
    report_edge_contact(a, b, on_a, on_b, outcome);
  }

  void contact(Side& sa, Side& sb, PairOutcome& outcome) const {
    const Prepared& a = *sa.p;
    const Prepared& b = *sb.p;
    auto dist = kernel::min_distance(*sa.boundary, *sb.boundary);
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
        report_contact(a, b, fa.name, fb.name, area, kernel::bounding_box(*shared), outcome);
      }
    }
    if (any) return;
    report_edge_contact(a, b, dist->point_on_a, dist->point_on_b, outcome);
  }
};

// --- connection rules ------------------------------------------------------------

double box_distance(const dbox3& b, const dvec3& p) {
  const dvec3 d{std::max({b.min.x - p.x, 0.0, p.x - b.max.x}),
                std::max({b.min.y - p.y, 0.0, p.y - b.max.y}),
                std::max({b.min.z - p.z, 0.0, p.z - b.max.z})};
  return length(d);
}

// The distance between two boxes.
double box_gap(const dbox3& a, const dbox3& b) {
  const dvec3 d{std::max({a.min.x - b.max.x, 0.0, b.min.x - a.max.x}),
                std::max({a.min.y - b.max.y, 0.0, b.min.y - a.max.y}),
                std::max({a.min.z - b.max.z, 0.0, b.min.z - a.max.z})};
  return length(d);
}

// The distance from a rectangle (or any box) to the surface of a box, as
// kernel::min_distance() measures a patch's distance to a solid: from
// outside, the gap between them; from wholly inside, how near it comes to
// a side; crossing the surface, 0.
double surface_gap(const dbox3& patch, const dbox3& box) {
  const double gap = box_gap(patch, box);
  if (gap > 0.0) return gap;
  const double depth = std::min({patch.min.x - box.min.x, box.max.x - patch.max.x,
                                 patch.min.y - box.min.y, box.max.y - patch.max.y,
                                 patch.min.z - box.min.z, box.max.z - patch.max.z});
  return std::max(depth, 0.0);
}

// A patch that is exactly its bounding box: planar on an axis-aligned
// plane, as large as the rectangle its box spans (no holes, no curved or
// slanted sides).
struct Rectangle {
  dbox3 box;
  int axis = 0;  // of its normal
};

std::optional<Rectangle> rectangle_of(const Face& patch, double tol) {
  const auto plane = kernel::face_surface_plane(patch);
  if (!plane) return std::nullopt;
  const dvec3 n = normalize(plane->normal);
  const dbox3 box = kernel::bounding_box(patch);
  const dvec3 size = box.size();
  double rectangle = 0.0;
  int axis = 0;
  if (std::abs(n.x) > 1.0 - 1e-12 && size.x <= tol) {
    rectangle = size.y * size.z;
  } else if (std::abs(n.y) > 1.0 - 1e-12 && size.y <= tol) {
    rectangle = size.z * size.x;
    axis = 1;
  } else if (std::abs(n.z) > 1.0 - 1e-12 && size.z <= tol) {
    rectangle = size.x * size.y;
    axis = 2;
  } else {
    return std::nullopt;
  }
  const double area = kernel::surface_area(patch);
  if (!(rectangle > 0.0) || std::abs(area - rectangle) > 1e-9 * rectangle) return std::nullopt;
  return Rectangle{box, axis};
}

// How far a point sampled on a patch may stray from the patch's bounding
// box: the rounding of evaluating a plane at (u, v), far below this within
// the working space.
constexpr double kSampleSlack = 1e-9;

// Whether no point on `patch` can lie deeper inside `box` than the
// tolerance: it lies within the tolerance (less the sampling slack) of
// one of the box's two sides across its normal. Every point's depth is at
// most its distance to either of those sides, so deepest_inside_box() of
// any points on it would be 0.
bool on_a_side(const Rectangle& patch, const dbox3& box, double tol) {
  const auto at = [](const dvec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; };
  const int a = patch.axis;
  return at(patch.box.max, a) + kSampleSlack - at(box.min, a) <= tol ||
         at(box.max, a) - (at(patch.box.min, a) - kSampleSlack) <= tol;
}

// deepest_inside() for a boundary that is exactly `box`: a point more than
// the tolerance inside every side is inside, and its depth is its distance
// to the nearest side.
double deepest_inside_box(const std::vector<dvec3>& points, const dbox3& box, double tol) {
  double worst = 0.0;
  for (const dvec3& p : points) {
    const double depth = std::min({p.x - box.min.x, box.max.x - p.x, p.y - box.min.y,
                                   box.max.y - p.y, p.z - box.min.z, box.max.z - p.z});
    if (depth > tol) worst = std::max(worst, depth);
  }
  return worst;
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
                             const ProductSettings& settings, const ParallelFor& parallel,
                             const BoxBoundaries& boxes, bool sample_every_patch) {
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
    // A rectangle on an axis-aligned plane is its bounding box: its
    // distance to a box boundary is the boxes' (decision 0032).
    const std::optional<Rectangle> rectangle = rectangle_of(cb->patch, tol);
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
      const auto box = boxes.find(vb);
      const bool box_path = box != boxes.end() && rectangle.has_value();
      const kernel::Solid* boundary = box_path ? &vb->boundary : boundary_of(*vb);
      if (boundary == nullptr) {
        undecided("copying its boundary failed");
        continue;
      }
      double off = 0.0;
      if (box_path) {
        off = surface_gap(rectangle->box, box->second);
      } else {
        auto d = kernel::min_distance(cb->patch, *boundary);
        if (!d) {
          undecided(d.error().message);
          continue;
        }
        off = d->distance;
      }
      bool inside = false;
      if (off <= reach && box_path && !sample_every_patch &&
          on_a_side(*rectangle, box->second, tol)) {
        // No point of it can lie deeper inside than the tolerance, so
        // deepest_inside_box() of its samples would be 0 (decision 0034).
      } else if (off <= reach) {
        if (!samples) {
          auto s = patch_points(cb->patch, settings);
          if (!s) {
            undecided(s.error().message);
            continue;
          }
          samples = std::move(*s);
        }
        auto far = box_path ? core::Result<double>(deepest_inside_box(*samples, box->second, tol))
                            : deepest_inside(*samples, *boundary, settings, reach);
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
  BoxBoundaries boxes;
  for (const auto& vb : build.volumes) {
    if (vb.boundary.is_empty()) continue;
    const dbox3 box = kernel::bounding_box(vb.boundary);
    if (box_sides(vb.boundary, box, settings.linear_tolerance_mm)) boxes.emplace(&vb, box);
  }
  return connection_rules(model::DocumentIndex(document), build, settings, parallel, boxes,
                          false);
}

LayoutReport validate_layout(const Document& document, const DocumentBuild& build,
                             const ProductSettings& settings, const ParallelFor& parallel,
                             const LayoutOptions& options) {
  const model::DocumentIndex index(document);
  return LayoutChecker(document, index, build, settings, parallel, options).run();
}

}  // namespace vs::validate

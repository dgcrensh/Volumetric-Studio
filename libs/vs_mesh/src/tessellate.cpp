#include "vs/mesh/tessellate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "vs/mesh/topology.hpp"
#include "vs/mesh/triangulate.hpp"

namespace vs::mesh {
namespace {

using core::Diagnostic;
using core::dvec2;
using core::dvec3;
using core::Id;
using core::SubjectRef;
using kernel::TessEdge;
using kernel::TessEdgeUse;
using kernel::TessFace;
using kernel::Tessellation;
using kernel::TessNode;

constexpr std::uint32_t kNone = 0xffffffffu;

dvec2 project(const sketch::SketchPlane& frame, const dvec3& p) {
  const dvec3 d = p - frame.origin;
  return {core::dot(d, frame.x_axis), core::dot(d, frame.y_axis)};
}

double point_segment_distance(const dvec2& q, const dvec2& a, const dvec2& b) {
  const dvec2 ab = b - a;
  const double len2 = core::dot(ab, ab);
  double t = len2 > 0 ? core::dot(q - a, ab) / len2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  return core::length(q - (a + ab * t));
}

// Distance from q to an exact outline segment.
double outline_distance(const OutlineSegment& s, const dvec2& q) {
  if (s.kind == OutlineSegment::Kind::Line) return point_segment_distance(q, s.start, s.end);
  const dvec2 d = q - s.centre;
  const double r = core::length(d);
  // Within the swept angle? Measure from the start, in the sweep's direction.
  double a = std::atan2(d.y, d.x) - s.start_angle;
  const double two_pi = 2.0 * 3.14159265358979323846;
  if (s.sweep < 0) a = -a;
  a = std::fmod(std::fmod(a, two_pi) + two_pi, two_pi);
  if (a <= std::abs(s.sweep)) return std::abs(r - s.radius);
  return std::min(core::length(q - s.start), core::length(q - s.end));
}

// Even-odd point in polygon over several loops.
bool inside(const std::vector<std::vector<dvec2>>& loops, const dvec2& q) {
  bool in = false;
  for (const auto& loop : loops) {
    for (std::size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
      const dvec2& a = loop[i];
      const dvec2& b = loop[j];
      if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x) {
        in = !in;
      }
    }
  }
  return in;
}

double loops_distance(const std::vector<std::vector<dvec2>>& loops, const dvec2& q) {
  double best = std::numeric_limits<double>::infinity();
  for (const auto& loop : loops) {
    for (std::size_t i = 0; i < loop.size(); ++i) {
      best = std::min(best, point_segment_distance(q, loop[i], loop[(i + 1) % loop.size()]));
    }
  }
  return best;
}

struct Box2 {
  dvec2 lo{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
  dvec2 hi{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
  void expand(const dvec2& p) {
    lo = core::min(lo, p);
    hi = core::max(hi, p);
  }
  [[nodiscard]] Box2 expanded(double d) const { return {lo - dvec2{d, d}, hi + dvec2{d, d}}; }
  [[nodiscard]] bool contains(const dvec2& p) const {
    return lo.x <= p.x && p.x <= hi.x && lo.y <= p.y && p.y <= hi.y;
  }
  [[nodiscard]] bool intersects(const Box2& o) const {
    return lo.x <= o.hi.x && o.lo.x <= hi.x && lo.y <= o.hi.y && o.lo.y <= hi.y;
  }
};

// One seam, prepared for lookups.
struct SeamInfo {
  const Seam* seam = nullptr;
  std::vector<std::vector<dvec2>> loops_uv;  // outline loops, in the seam's frame
  Box2 box;                                 // of the outline, in the frame
  std::vector<std::uint32_t> outline;        // every outline vertex index
  bool reported = false;                     // uncovered already said
  bool placed = false;
};

struct PolyPoint {
  double t = 0;
  std::uint32_t node = 0;
};

class Tessellator {
 public:
  Tessellator(const Tessellation& t, const SubjectRef& volume, std::span<const Seam* const> seams,
              const core::ProductSettings& settings, VolumeMesh& out)
      : t_(t), volume_(volume), out_(out), tol_(10.0 * settings.linear_tolerance_mm) {
    for (const Seam* s : seams) {
      SeamInfo info;
      info.seam = s;
      for (const SeamLoop& loop : s->loops) {
        std::vector<dvec2> uv;
        for (const std::uint32_t v : loop.vertices) {
          uv.push_back(s->vertices[v].uv);
          info.box.expand(s->vertices[v].uv);
          info.outline.push_back(v);
        }
        info.loops_uv.push_back(std::move(uv));
      }
      // Arcs bulge past their chords by less than a piece's length.
      info.box = info.box.expanded(s->spacing);
      seams_.push_back(std::move(info));
    }
  }

  void run() {
    // Vertex nodes first, so a vertex's node index is the vertex's.
    for (const dvec3& p : t_.vertices) add_node(p);
    bound_.resize(t_.vertices.size());
    for (std::uint32_t v = 0; v < bound_.size(); ++v) bound_[v] = v;
    bind_vertices();
    divide_edges();
    for (std::uint32_t f = 0; f < t_.faces.size(); ++f) mesh_face(f);
    for (SeamInfo& s : seams_) {
      if (s.placed) {
        out_.seams.push_back(s.seam->surface);
      } else {
        uncovered(s, "No face of the volume covers the surface's patch");
      }
    }
    assemble();
    if (snapped_ > 0) {
      out_.diagnostics.add(Diagnostic::info(
          "mesh.tessellate.snapped",
          std::to_string(snapped_) +
              " vertices of the volume lie on a seam's outline between its vertices and were "
              "moved to the nearest one",
          {volume_}));
    }
  }

 private:
  struct Node {
    dvec3 position{};
    std::uint32_t seam = kNone;  // index into seams_
    std::uint32_t seam_vertex = 0;
  };

  std::uint32_t add_node(const dvec3& p) {
    nodes_.push_back({p});
    return static_cast<std::uint32_t>(nodes_.size() - 1);
  }
  std::uint32_t seam_node(std::uint32_t s, std::uint32_t v) {
    const std::uint64_t key = (static_cast<std::uint64_t>(s) << 32) | v;
    auto [it, added] = seam_nodes_.try_emplace(key, 0);
    if (added) {
      it->second = add_node(seams_[s].seam->vertices[v].position);
      nodes_.back().seam = s;
      nodes_.back().seam_vertex = v;
    }
    return it->second;
  }
  std::uint32_t edge_node(std::uint32_t e, std::uint32_t k) {
    const std::uint64_t key = (static_cast<std::uint64_t>(e) << 32) | k;
    auto [it, added] = edge_nodes_.try_emplace(key, 0);
    if (added) it->second = add_node(t_.edges[e].points[k]);
    return it->second;
  }

  void uncovered(SeamInfo& s, const std::string& what) {
    if (s.reported) return;
    s.reported = true;
    out_.diagnostics.add(Diagnostic::error("mesh.tessellate.seam_uncovered",
                                           what + "; the volume can't conform to its seam",
                                           {s.seam->subject(), volume_}));
  }

  // 1. B-rep vertices on a seam's outline become its vertices.
  void bind_vertices() {
    for (std::uint32_t s = 0; s < seams_.size(); ++s) {
      const Seam& seam = *seams_[s].seam;
      const dvec3 n = seam.normal();
      for (std::uint32_t v = 0; v < t_.vertices.size(); ++v) {
        const dvec3& p = t_.vertices[v];
        if (std::abs(core::dot(p - seam.frame.origin, n)) > tol_) continue;
        const dvec2 q = project(seam.frame, p);
        if (!seams_[s].box.expanded(tol_).contains(q)) continue;
        std::optional<std::uint32_t> hit;
        for (const std::uint32_t j : seams_[s].outline) {
          if (core::length(seam.vertices[j].uv - q) <= tol_) {
            hit = j;
            break;
          }
        }
        if (!hit) {
          // On the outline between seam vertices: the nearest one of its loop.
          for (std::size_t l = 0; l < seam.loops.size() && !hit; ++l) {
            for (const OutlineSegment& seg : seam.outline.loops[l].segments) {
              if (outline_distance(seg, q) > tol_) continue;
              double best = std::numeric_limits<double>::infinity();
              for (const std::uint32_t j : seam.loops[l].vertices) {
                const double d = core::length(seam.vertices[j].uv - q);
                if (d < best) {
                  best = d;
                  hit = j;
                }
              }
              ++snapped_;
              break;
            }
          }
        }
        if (!hit) continue;
        if (bound_[v] != v) {
          out_.diagnostics.add(Diagnostic::warning(
              "mesh.tessellate.seam_conflict",
              "A vertex of the volume lies on two seams; it stays on the first",
              {volume_, seam.subject()}));
          continue;
        }
        bound_[v] = seam_node(s, *hit);
      }
    }
  }

  // 2. Edge divisions: BRepMesh's, or the seams' along their outlines.
  void divide_edges() {
    polylines_.resize(t_.edges.size());
    modified_.assign(t_.edges.size(), false);
    for (std::uint32_t e = 0; e < t_.edges.size(); ++e) {
      const TessEdge& te = t_.edges[e];
      auto& poly = polylines_[e];
      const std::size_t n = te.params.size();
      for (std::size_t k = 0; k < n; ++k) {
        std::uint32_t node = 0;
        if (k == 0 || te.degenerate) {
          node = bound_[te.first_vertex];
        } else if (k + 1 == n) {
          node = bound_[te.last_vertex];
        } else {
          node = edge_node(e, static_cast<std::uint32_t>(k));
        }
        poly.push_back({te.params[k], node});
      }
      if (te.degenerate) continue;
      for (std::uint32_t s = 0; s < seams_.size(); ++s) divide_on_seam(e, s);
    }
  }

  void divide_on_seam(std::uint32_t e, std::uint32_t s) {
    const TessEdge& te = t_.edges[e];
    const Seam& seam = *seams_[s].seam;
    const dvec3 n = seam.normal();
    Box2 box;
    for (const dvec3& p : te.points) {
      if (std::abs(core::dot(p - seam.frame.origin, n)) > tol_) return;
      box.expand(project(seam.frame, p));
    }
    // The edge may bulge between its points (an arc): widen by its chord sag.
    double longest = 0;
    for (std::size_t k = 1; k < te.points.size(); ++k) {
      longest = std::max(longest, core::length(te.points[k] - te.points[k - 1]));
    }
    box = box.expanded(tol_ + 0.5 * longest);
    if (!box.intersects(seams_[s].box)) return;

    auto& poly = polylines_[e];
    const double first = te.params.front(), last = te.params.back();
    const std::uint32_t first_node = poly.front().node, last_node = poly.back().node;
    // Where each outline vertex lies on the edge (ends included).
    std::unordered_map<std::uint32_t, std::vector<double>> on_edge;
    std::vector<PolyPoint> hits;
    for (const std::uint32_t j : seams_[s].outline) {
      const SeamVertex& sv = seam.vertices[j];
      if (!box.contains(sv.uv)) continue;
      const std::uint32_t node = seam_node(s, j);
      if (node == first_node) on_edge[j].push_back(first);
      if (node == last_node) on_edge[j].push_back(last);
      if (node == first_node || node == last_node) continue;
      const std::optional<double> at = kernel::edge_parameter(te.edge, sv.position, tol_);
      if (!at) continue;
      on_edge[j].push_back(*at);
      hits.push_back({*at, node});
    }
    if (on_edge.empty()) return;
    // Outline pieces running along the edge.
    std::vector<std::pair<double, double>> along;
    for (std::size_t l = 0; l < seam.loops.size(); ++l) {
      const SeamLoop& loop = seam.loops[l];
      for (std::size_t i = 0; i < loop.vertices.size(); ++i) {
        const auto a = on_edge.find(loop.vertices[i]);
        const auto b = on_edge.find(loop.vertices[(i + 1) % loop.vertices.size()]);
        if (a == on_edge.end() || b == on_edge.end()) continue;
        // The piece's exact midpoint, from its outline segment.
        const auto g = static_cast<std::size_t>(
            std::upper_bound(loop.segment_starts.begin(), loop.segment_starts.end(),
                             static_cast<std::uint32_t>(i)) -
            loop.segment_starts.begin() - 1);
        const std::uint32_t start = loop.segment_starts[g];
        const std::uint32_t end = g + 1 < loop.segment_starts.size()
                                      ? loop.segment_starts[g + 1]
                                      : static_cast<std::uint32_t>(loop.vertices.size());
        const double frac = (static_cast<double>(i - start) + 0.5) / static_cast<double>(end - start);
        const dvec2 mid = seam.outline.loops[l].segments[g].point(frac);
        const std::optional<double> tm =
            kernel::edge_parameter(te.edge, seam.frame.to_model(mid), tol_);
        if (!tm) continue;
        for (const double ta : a->second) {
          for (const double tb : b->second) {
            if (std::min(ta, tb) <= *tm && *tm <= std::max(ta, tb)) {
              along.emplace_back(std::min(ta, tb), std::max(ta, tb));
            }
          }
        }
      }
    }
    if (hits.empty() && along.empty()) return;

    // Keep the edge's own points off the outline and clear of the hits.
    std::vector<PolyPoint> next;
    next.push_back(poly.front());
    for (std::size_t k = 1; k + 1 < poly.size(); ++k) {
      const PolyPoint& p = poly[k];
      bool keep = true;
      for (const auto& [lo, hi] : along) {
        if (lo < p.t && p.t < hi) keep = false;
      }
      const dvec3& at = nodes_[p.node].position;
      const double gap = std::min(core::length(at - nodes_[poly[k - 1].node].position),
                                  core::length(at - nodes_[poly[k + 1].node].position));
      for (const PolyPoint& h : hits) {
        if (core::length(nodes_[h.node].position - at) < 0.25 * gap) keep = false;
      }
      if (keep) next.push_back(p);
    }
    for (const PolyPoint& h : hits) next.push_back(h);
    next.push_back(poly.back());
    std::stable_sort(next.begin() + 1, next.end() - 1,
                     [](const PolyPoint& a, const PolyPoint& b) { return a.t < b.t; });
    // A hit that is also an end (a snapped vertex) appears once.
    std::vector<PolyPoint> clean;
    for (const PolyPoint& p : next) {
      if (!clean.empty() && clean.back().node == p.node) continue;
      clean.push_back(p);
    }
    if (clean.size() >= 2 && clean.back().node == clean[clean.size() - 2].node) clean.pop_back();
    if (clean.back().node != poly.back().node) clean.push_back(poly.back());
    poly = std::move(clean);
    modified_[e] = true;
  }

  // 3 and 4. One face.
  void mesh_face(std::uint32_t fi) {
    const TessFace& f = t_.faces[fi];
    std::vector<std::uint32_t> candidates;
    if (f.plane) {
      for (std::uint32_t s = 0; s < seams_.size(); ++s) {
        const Seam& seam = *seams_[s].seam;
        if (std::abs(core::dot(f.plane->normal, seam.normal())) < 1.0 - 1e-9) continue;
        if (std::abs(core::dot(f.plane->origin - seam.frame.origin, seam.normal())) > tol_) continue;
        candidates.push_back(s);
      }
    }
    bool changed = !candidates.empty();
    for (const auto& wire : f.wires) {
      for (const TessEdgeUse& u : wire) changed |= modified_[u.edge];
    }
    if (!changed) {
      keep_face(f);
      return;
    }
    if (!remesh_face(f, candidates)) keep_face(f);
  }

  // BRepMesh's triangles, joined to the shared vertex and edge points.
  void keep_face(const TessFace& f) {
    std::vector<std::uint32_t> map(f.points.size());
    for (std::size_t p = 0; p < f.points.size(); ++p) {
      const TessNode& n = f.nodes[p];
      switch (n.kind) {
        case TessNode::Kind::Vertex:
          map[p] = bound_[n.index];
          break;
        case TessNode::Kind::Edge: {
          // Unchanged division: its k-th point is BRepMesh's.
          const auto& poly = polylines_[n.index];
          map[p] = n.k < poly.size() && !modified_[n.index] ? poly[n.k].node
                                                            : edge_node(n.index, n.k);
          break;
        }
        default:
          map[p] = add_node(f.points[p]);
      }
    }
    for (const auto& tri : f.triangles) triangles_.push_back({map[tri[0]], map[tri[1]], map[tri[2]]});
  }

  bool remesh_face(const TessFace& f, const std::vector<std::uint32_t>& candidates) {
    const bool planar = !candidates.empty();
    const sketch::SketchPlane* frame = planar ? &seams_[candidates[0]].seam->frame : nullptr;
    const auto to2d = [&](std::uint32_t node) {
      const Node& n = nodes_[node];
      if (n.seam == candidates[0]) return seams_[n.seam].seam->vertices[n.seam_vertex].uv;
      return project(*frame, n.position);
    };

    std::vector<dvec2> pts;
    std::vector<std::uint32_t> point_node;
    std::vector<Loop> loops;
    std::vector<std::vector<dvec2>> loops2d;
    // Boundary pieces of edges whose division changed, in 2D.
    std::vector<std::pair<dvec2, dvec2>> changed;
    for (const auto& wire : f.wires) {
      Loop loop;
      std::vector<dvec2> loop2d;
      for (const TessEdgeUse& u : wire) {
        std::vector<PolyPoint> poly = polylines_[u.edge];
        if (u.reversed) std::reverse(poly.begin(), poly.end());
        std::vector<dvec2> q(poly.size());
        for (std::size_t k = 0; k < poly.size(); ++k) {
          if (planar) {
            q[k] = to2d(poly[k].node);
          } else {
            const dvec2 uv = kernel::edge_uv(f.face, u.oriented, poly[k].t);
            q[k] = {uv.x * f.uv_scale.x, uv.y * f.uv_scale.y};
          }
        }
        for (std::size_t k = 0; k + 1 < poly.size(); ++k) {
          loop.push_back(static_cast<std::uint32_t>(pts.size()));
          loop2d.push_back(q[k]);
          pts.push_back(q[k]);
          point_node.push_back(poly[k].node);
          if (modified_[u.edge]) changed.emplace_back(q[k], q[k + 1]);
        }
      }
      if (loop.size() >= 3) {
        loops.push_back(std::move(loop));
        loops2d.push_back(std::move(loop2d));
      }
    }
    if (loops.empty()) return false;
    const std::size_t loop_points = pts.size();

    // Seams this face carries, the extra segments they draw, and their holes.
    struct Placed {
      std::uint32_t seam;
      std::vector<bool> filled;  // per hole
    };
    std::vector<Placed> placed;
    std::vector<Segment> segments;
    if (planar) {
      std::unordered_map<std::uint32_t, std::uint32_t> node_point;
      for (std::uint32_t i = 0; i < loop_points; ++i) node_point.emplace(point_node[i], i);
      std::set<std::pair<std::uint32_t, std::uint32_t>> boundary;
      for (const Loop& loop : loops) {
        for (std::size_t i = 0; i < loop.size(); ++i) {
          const std::uint32_t a = point_node[loop[i]];
          const std::uint32_t b = point_node[loop[(i + 1) % loop.size()]];
          boundary.emplace(std::min(a, b), std::max(a, b));
        }
      }
      for (const std::uint32_t s : candidates) {
        SeamInfo& info = seams_[s];
        const Seam& seam = *info.seam;
        const auto in_frame = [&](const dvec2& uv) {
          return s == candidates[0] ? uv : project(*frame, seam.frame.to_model(uv));
        };
        std::size_t in = 0;
        for (const Triangle& tri : seam.triangles) {
          const dvec2 c = (seam.vertices[tri[0]].uv + seam.vertices[tri[1]].uv +
                           seam.vertices[tri[2]].uv) /
                          3.0;
          if (inside(loops2d, in_frame(c))) ++in;
        }
        if (in == 0) continue;
        if (in < seam.triangles.size()) {
          uncovered(info, "A face of the volume covers only part of the surface's patch");
          continue;
        }
        bool clear = true;
        for (std::uint32_t i = 0; i < loop_points && clear; ++i) {
          if (nodes_[point_node[i]].seam == s) continue;
          const dvec2 q = project(seam.frame, nodes_[point_node[i]].position);
          if (inside(info.loops_uv, q) && loops_distance(info.loops_uv, q) > tol_) clear = false;
        }
        if (!clear) {
          uncovered(info, "The volume's face has an edge inside the surface's patch");
          continue;
        }
        Placed p{s, std::vector<bool>(seam.loops.size() > 0 ? seam.loops.size() - 1 : 0, true)};
        for (std::size_t l = 0; l < seam.loops.size(); ++l) {
          const auto& lv = seam.loops[l].vertices;
          for (std::size_t i = 0; i < lv.size(); ++i) {
            const std::uint32_t a = seam_node(s, lv[i]);
            const std::uint32_t b = seam_node(s, lv[(i + 1) % lv.size()]);
            if (boundary.contains({std::min(a, b), std::max(a, b)})) {
              if (l > 0) p.filled[l - 1] = false;  // the face has this hole
              continue;
            }
            std::uint32_t ends[2] = {a, b};
            Segment seg{};
            for (int k = 0; k < 2; ++k) {
              auto [it, added] = node_point.try_emplace(ends[k], static_cast<std::uint32_t>(pts.size()));
              if (added) {
                pts.push_back(to2d(ends[k]));
                point_node.push_back(ends[k]);
              }
              seg[static_cast<std::size_t>(k)] = it->second;
            }
            segments.push_back(seg);
          }
          if (l > 0 && p.filled[l - 1]) {
            // A hole with the face's own boundary inside it isn't filled.
            for (std::uint32_t i = 0; i < loop_points; ++i) {
              if (nodes_[point_node[i]].seam == s) continue;
              const dvec2 q = project(seam.frame, nodes_[point_node[i]].position);
              if (inside({info.loops_uv[l]}, q) && loops_distance({info.loops_uv[l]}, q) > tol_) {
                p.filled[l - 1] = false;
                break;
              }
            }
          }
        }
        placed.push_back(std::move(p));
      }
    } else {
      // BRepMesh's interior points, inside the face and away from the
      // pieces of boundary whose division changed (elsewhere they sit as
      // BRepMesh put them, against the same boundary).
      for (std::size_t p = 0; p < f.points.size(); ++p) {
        if (f.nodes[p].kind != TessNode::Kind::Interior) continue;
        const dvec2 q{f.uv[p].x * f.uv_scale.x, f.uv[p].y * f.uv_scale.y};
        if (!inside(loops2d, q)) continue;
        bool keep = true;
        for (const auto& [a, b] : changed) {
          if (point_segment_distance(q, a, b) < 0.3 * core::length(b - a)) {
            keep = false;
            break;
          }
        }
        if (keep && loops_distance(loops2d, q) < 1e-9 * (1.0 + core::length(q))) keep = false;
        if (!keep) continue;
        pts.push_back(q);
        point_node.push_back(add_node(f.points[p]));
      }
    }

    const core::Result<std::vector<Triangle>> tris = triangulate(pts, loops, segments);
    if (!tris) {
      out_.diagnostics.add(Diagnostic::error(
          "mesh.tessellate.face_failed",
          "A face couldn't be re-triangulated against the seams (" + tris.error().code + ": " +
              tris.error().message + ")",
          {volume_}));
      return false;
    }
    bool flip = false;
    if (planar) {
      flip = core::dot(f.plane->normal, frame->normal()) < 0;
    } else {
      flip = !f.uv_outward;
    }
    for (const Triangle& tri : *tris) {
      if (!placed.empty()) {
        const dvec2 c = (pts[tri[0]] + pts[tri[1]] + pts[tri[2]]) / 3.0;
        bool drop = false;
        for (const Placed& p : placed) {
          const SeamInfo& info = seams_[p.seam];
          const dvec2 cs =
              p.seam == candidates[0] ? c : project(info.seam->frame, frame->to_model(c));
          if (inside(info.loops_uv, cs)) {
            drop = true;  // the seam's own triangles go here
            break;
          }
          for (std::size_t h = 0; h < p.filled.size(); ++h) {
            if (p.filled[h] && inside({info.loops_uv[h + 1]}, cs)) drop = true;
          }
          if (drop) break;
        }
        if (drop) continue;
      }
      std::array<std::uint32_t, 3> t{point_node[tri[0]], point_node[tri[1]], point_node[tri[2]]};
      if (flip) std::swap(t[1], t[2]);
      triangles_.push_back(t);
    }
    for (const Placed& p : placed) {
      SeamInfo& info = seams_[p.seam];
      const Seam& seam = *info.seam;
      const bool turn = core::dot(f.plane->normal, seam.normal()) < 0;
      const auto add = [&](const Triangle& tri) {
        std::array<std::uint32_t, 3> t{seam_node(p.seam, tri[0]), seam_node(p.seam, tri[1]),
                                       seam_node(p.seam, tri[2])};
        if (turn) std::swap(t[1], t[2]);
        triangles_.push_back(t);
      };
      for (const Triangle& tri : seam.triangles) add(tri);
      for (std::size_t h = 0; h < p.filled.size(); ++h) {
        if (!p.filled[h]) continue;
        for (const Triangle& tri : seam.hole_fills[h]) add(tri);
      }
      info.placed = true;
    }
    return true;
  }

  void assemble() {
    Mesh& m = out_.mesh;
    std::vector<std::uint32_t> vertex(nodes_.size(), kNone);
    const auto vertex_of = [&](std::uint32_t node) {
      if (vertex[node] == kNone) {
        const Node& n = nodes_[node];
        if (n.seam != kNone) {
          const SeamVertex& sv = seams_[n.seam].seam->vertices[n.seam_vertex];
          vertex[node] = m.add_vertex(sv.position32, sv.id);
        } else {
          vertex[node] = m.add_vertex(core::to_fp32(n.position));
        }
      }
      return vertex[node];
    };
    for (const auto& t : triangles_) {
      if (t[0] == t[1] || t[1] == t[2] || t[2] == t[0]) continue;  // at a pole, or snapped together
      m.add_triangle(vertex_of(t[0]), vertex_of(t[1]), vertex_of(t[2]), volume_.id);
    }
    const TopologyReport report = analyse(m);
    if (!report.watertight() || !report.manifold()) {
      out_.diagnostics.add(Diagnostic::error(
          "mesh.tessellate.open",
          "The volume's mesh isn't closed and manifold (" +
              std::to_string(report.boundary_edges.size()) + " open edges, " +
              std::to_string(report.non_manifold_edges.size() + report.inconsistent_edges.size() +
                             report.non_manifold_vertices.size()) +
              " other defects)",
          {volume_}));
    }
  }

  const Tessellation& t_;
  SubjectRef volume_;
  VolumeMesh& out_;
  double tol_;
  std::vector<SeamInfo> seams_;
  std::vector<Node> nodes_;
  std::vector<std::uint32_t> bound_;  // per B-rep vertex: its node
  std::unordered_map<std::uint64_t, std::uint32_t> seam_nodes_;
  std::unordered_map<std::uint64_t, std::uint32_t> edge_nodes_;
  std::vector<std::vector<PolyPoint>> polylines_;
  std::vector<bool> modified_;
  std::vector<std::array<std::uint32_t, 3>> triangles_;
  std::size_t snapped_ = 0;
};

}  // namespace

VolumeMesh tessellate(const kernel::Solid& solid, const core::SubjectRef& volume,
                      std::span<const Seam* const> seams, const core::ProductSettings& settings,
                      const TessellationOptions& options) {
  VolumeMesh out;
  const core::Result<Tessellation> t = kernel::tessellate(solid, options.params, settings);
  if (!t) {
    Diagnostic d = t.error();
    d.subjects.insert(d.subjects.begin(), volume);
    out.diagnostics.add(std::move(d));
    return out;
  }
  Tessellator(*t, volume, seams, settings, out).run();
  return out;
}

}  // namespace vs::mesh

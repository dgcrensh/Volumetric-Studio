#include "vs/draw/view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <unordered_map>

#include <fmt/format.h>

#include "vs/kernel/tessellate.hpp"

namespace vs::draw {

using core::Diagnostic;
using core::Diagnostics;

ViewFrame view_frame(model::ViewDirection direction) {
  using model::ViewDirection;
  switch (direction) {
    case ViewDirection::Front: return {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
    case ViewDirection::Back: return {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}};
    case ViewDirection::Top: return {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    case ViewDirection::Bottom: return {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    case ViewDirection::Right: return {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}};
    case ViewDirection::Left: return {{0, -1, 0}, {0, 0, 1}, {-1, 0, 0}};
    case ViewDirection::Isometric: {
      const double r2 = std::sqrt(2.0);
      const double r3 = std::sqrt(3.0);
      const double r6 = std::sqrt(6.0);
      return {{1 / r2, 1 / r2, 0}, {-1 / r6, 1 / r6, 2 / r6}, {1 / r3, -1 / r3, 1 / r3}};
    }
  }
  return {};
}

namespace {

using Line3 = std::vector<dvec3>;

// A segment between two vertices of a numbered point set, for chaining.
struct Segment {
  std::uint32_t a = 0;
  std::uint32_t b = 0;
};

// Joins segments sharing vertices into polylines: a chain runs on through
// every vertex that exactly two segments meet at. Deterministic: chains
// start from the first unused segment, in order.
std::vector<Line3> chain(const std::vector<Segment>& segments, const std::vector<dvec3>& points) {
  std::vector<Line3> out;
  if (segments.empty()) return out;
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> at;
  for (std::uint32_t s = 0; s < segments.size(); ++s) {
    at[segments[s].a].push_back(s);
    at[segments[s].b].push_back(s);
  }
  std::vector<bool> used(segments.size(), false);
  auto next = [&](std::uint32_t vertex, std::uint32_t from) -> std::optional<std::uint32_t> {
    const auto& list = at[vertex];
    if (list.size() != 2) return std::nullopt;
    const std::uint32_t s = list[0] == from ? list[1] : list[0];
    if (used[s]) return std::nullopt;
    return s;
  };
  for (std::uint32_t s0 = 0; s0 < segments.size(); ++s0) {
    if (used[s0]) continue;
    used[s0] = true;
    std::vector<std::uint32_t> ids{segments[s0].a, segments[s0].b};
    // Forward from b, then backward from a.
    for (int dir = 0; dir < 2; ++dir) {
      std::uint32_t s = s0;
      std::uint32_t v = dir == 0 ? segments[s0].b : segments[s0].a;
      std::vector<std::uint32_t> more;
      while (auto n = next(v, s)) {
        s = *n;
        used[s] = true;
        v = segments[s].a == v ? segments[s].b : segments[s].a;
        more.push_back(v);
      }
      if (dir == 0) {
        ids.insert(ids.end(), more.begin(), more.end());
      } else {
        ids.insert(ids.begin(), more.rbegin(), more.rend());
      }
    }
    Line3 line;
    line.reserve(ids.size());
    for (auto id : ids) line.push_back(points[id]);
    out.push_back(std::move(line));
  }
  return out;
}

struct Triangles {
  std::vector<dvec3> points;
  std::vector<std::array<std::uint32_t, 3>> triangles;
};

struct Extracted {
  std::vector<Line3> lines;
  Triangles occluders;  // contents only
};

dvec3 normal_of(const std::vector<dvec3>& p, const std::array<std::uint32_t, 3>& t) {
  return cross(p[t[1]] - p[t[0]], p[t[2]] - p[t[0]]);
}

// Turned towards the viewer, by more than edge-on: a facet seen edge-on
// counts as turned away, so a silhouette runs along the front edge of it.
bool faces(const dvec3& normal, const dvec3& toward) {
  return dot(normal, toward) > 1e-6 * core::length(normal);
}

// Silhouette segments of one triangulated piece: triangle edges shared by
// two triangles, one turned towards the viewer and one away. `ids` number
// the points so that points shared across a closed surface's seam match.
void silhouettes(const std::vector<dvec3>& points, const std::vector<std::uint32_t>& ids,
                 const std::vector<std::array<std::uint32_t, 3>>& triangles,
                 const dvec3& toward, std::vector<Segment>& out) {
  struct EdgeUse {
    std::uint64_t key;
    std::uint32_t triangle;
    std::uint32_t a, b;  // point indices
  };
  std::vector<EdgeUse> uses;
  uses.reserve(triangles.size() * 3);
  for (std::uint32_t t = 0; t < triangles.size(); ++t) {
    for (int k = 0; k < 3; ++k) {
      const std::uint32_t a = triangles[t][static_cast<std::size_t>(k)];
      const std::uint32_t b = triangles[t][static_cast<std::size_t>((k + 1) % 3)];
      const std::uint32_t ia = ids[a];
      const std::uint32_t ib = ids[b];
      if (ia == ib) continue;
      const std::uint64_t key =
          (static_cast<std::uint64_t>(std::min(ia, ib)) << 32) | std::max(ia, ib);
      uses.push_back({key, t, a, b});
    }
  }
  std::sort(uses.begin(), uses.end(), [](const EdgeUse& x, const EdgeUse& y) {
    return x.key != y.key ? x.key < y.key : x.triangle < y.triangle;
  });
  for (std::size_t i = 0; i < uses.size();) {
    std::size_t j = i;
    while (j < uses.size() && uses[j].key == uses[i].key) ++j;
    if (j - i == 2) {
      const bool f0 = faces(normal_of(points, triangles[uses[i].triangle]), toward);
      const bool f1 = faces(normal_of(points, triangles[uses[i + 1].triangle]), toward);
      if (f0 != f1) out.push_back({ids[uses[i].a], ids[uses[i].b]});
    }
    i = j;
  }
}

core::Result<Extracted> extract_shape(const kernel::Shape& shape, const ViewFrame& frame,
                                      const ProjectionOptions& options,
                                      const core::ProductSettings& settings, bool occluders) {
  kernel::TessellationParams params;
  params.deflection = options.deflection;
  params.angle = 0.2;
  auto tess = kernel::tessellate(shape, params, settings);
  if (!tess) return tess.error();
  Extracted out;

  // Seams: edges a face's wires use twice.
  std::vector<bool> seam(tess->edges.size(), false);
  for (const auto& f : tess->faces) {
    std::unordered_map<std::uint32_t, int> uses;
    for (const auto& w : f.wires) {
      for (const auto& u : w) ++uses[u.edge];
    }
    for (const auto& [e, n] : uses) {
      if (n >= 2) seam[e] = true;
    }
  }
  for (std::size_t e = 0; e < tess->edges.size(); ++e) {
    const auto& edge = tess->edges[e];
    if (edge.degenerate || seam[e] || edge.points.size() < 2) continue;
    out.lines.push_back(edge.points);
  }

  // Silhouettes, face by face. Points on vertices and edges are numbered by
  // what they lie on, so a face's two sides of its seam meet.
  std::vector<dvec3> all_points;
  std::unordered_map<std::uint64_t, std::uint32_t> numbered;
  std::vector<Segment> segments;
  for (const auto& f : tess->faces) {
    std::vector<std::uint32_t> ids(f.points.size());
    for (std::size_t i = 0; i < f.points.size(); ++i) {
      const auto& n = f.nodes[i];
      std::optional<std::uint64_t> key;
      if (n.kind == kernel::TessNode::Kind::Vertex) {
        key = (std::uint64_t{1} << 62) | n.index;
      } else if (n.kind == kernel::TessNode::Kind::Edge) {
        key = (std::uint64_t{2} << 62) | (static_cast<std::uint64_t>(n.index) << 32) | n.k;
      }
      if (key) {
        const auto [it, fresh] =
            numbered.try_emplace(*key, static_cast<std::uint32_t>(all_points.size()));
        if (fresh) all_points.push_back(f.points[i]);
        ids[i] = it->second;
      } else {
        ids[i] = static_cast<std::uint32_t>(all_points.size());
        all_points.push_back(f.points[i]);
      }
    }
    if (!f.plane) silhouettes(f.points, ids, f.triangles, frame.toward, segments);
    if (occluders) {
      const auto base = static_cast<std::uint32_t>(out.occluders.points.size());
      out.occluders.points.insert(out.occluders.points.end(), f.points.begin(), f.points.end());
      for (const auto& t : f.triangles) {
        out.occluders.triangles.push_back({base + t[0], base + t[1], base + t[2]});
      }
    }
  }
  for (auto& l : chain(segments, all_points)) out.lines.push_back(std::move(l));
  return out;
}

Extracted extract_mesh(const mesh::Mesh& m, const ViewFrame& frame,
                       const ProjectionOptions& options) {
  Extracted out;
  // Weld equal positions, so meshes read without shared vertices (STL)
  // still have neighbours.
  const std::size_t nv = m.positions.size();
  std::vector<std::uint32_t> order(nv);
  std::iota(order.begin(), order.end(), 0u);
  auto less = [&](std::uint32_t a, std::uint32_t b) {
    const auto& p = m.positions[a];
    const auto& q = m.positions[b];
    if (p.x != q.x) return p.x < q.x;
    if (p.y != q.y) return p.y < q.y;
    if (p.z != q.z) return p.z < q.z;
    return a < b;
  };
  std::sort(order.begin(), order.end(), less);
  std::vector<std::uint32_t> id(nv, 0);
  std::vector<dvec3> points;
  for (std::size_t i = 0; i < nv; ++i) {
    const auto v = order[i];
    if (i == 0 || !(m.positions[order[i - 1]] == m.positions[v])) {
      points.push_back(core::to_fp64(m.positions[v]));
    }
    id[v] = static_cast<std::uint32_t>(points.size() - 1);
  }
  std::vector<std::array<std::uint32_t, 3>> tris;
  tris.reserve(m.triangle_count());
  for (std::size_t t = 0; t < m.triangle_count(); ++t) {
    const auto tri = m.triangle(t);
    tris.push_back({id[tri[0]], id[tri[1]], id[tri[2]]});
  }
  std::vector<dvec3> normals(tris.size());
  for (std::size_t t = 0; t < tris.size(); ++t) normals[t] = normal_of(points, tris[t]);

  struct EdgeUse {
    std::uint64_t key;
    std::uint32_t triangle;
  };
  std::vector<EdgeUse> uses;
  uses.reserve(tris.size() * 3);
  for (std::uint32_t t = 0; t < tris.size(); ++t) {
    for (int k = 0; k < 3; ++k) {
      const auto a = tris[t][static_cast<std::size_t>(k)];
      const auto b = tris[t][static_cast<std::size_t>((k + 1) % 3)];
      if (a == b) continue;
      uses.push_back({(static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b), t});
    }
  }
  std::sort(uses.begin(), uses.end(), [](const EdgeUse& x, const EdgeUse& y) {
    return x.key != y.key ? x.key < y.key : x.triangle < y.triangle;
  });
  const double cos_crease = std::cos(options.crease_angle);
  std::vector<Segment> segments;
  for (std::size_t i = 0; i < uses.size();) {
    std::size_t j = i;
    while (j < uses.size() && uses[j].key == uses[i].key) ++j;
    bool draw = j - i != 2;  // borders and non-manifold edges
    if (!draw) {
      const dvec3& n0 = normals[uses[i].triangle];
      const dvec3& n1 = normals[uses[i + 1].triangle];
      const bool f0 = faces(n0, frame.toward);
      const bool f1 = faces(n1, frame.toward);
      const double l = core::length(n0) * core::length(n1);
      draw = f0 != f1 || (l > 0 && dot(n0, n1) < cos_crease * l);
    }
    if (draw) {
      segments.push_back({static_cast<std::uint32_t>(uses[i].key >> 32),
                          static_cast<std::uint32_t>(uses[i].key & 0xFFFFFFFFu)});
    }
    i = j;
  }
  out.lines = chain(segments, points);
  out.occluders.points = std::move(points);
  out.occluders.triangles = std::move(tris);
  return out;
}

// --- visibility -------------------------------------------------------------------------

class Occlusion {
 public:
  Occlusion(const std::vector<Triangles>& sets, const ViewFrame& frame) {
    for (const auto& s : sets) {
      const auto base = static_cast<std::uint32_t>(p_.size());
      for (const auto& q : s.points) {
        p_.push_back(frame.project(q));
        z_.push_back(frame.depth(q));
      }
      for (const auto& t : s.triangles) {
        const dvec2 a = p_[base + t[0]];
        const dvec2 b = p_[base + t[1]];
        const dvec2 c = p_[base + t[2]];
        const double area = cross(b - a, c - a);
        // Edge-on, or nearly: hides nothing, and its depths are ill
        // conditioned. The projected area is the area times the cosine of
        // the angle to the viewer.
        const double full = core::length(normal_of(s.points, t));
        if (!(std::abs(area) > 1e-6 * full)) continue;
        tri_.push_back({base + t[0], base + t[1], base + t[2]});
      }
    }
    if (tri_.empty()) return;
    for (const auto& q : p_) {
      lo_ = core::min(lo_, q);
      hi_ = core::max(hi_, q);
    }
    const double w = hi_.x - lo_.x;
    const double h = hi_.y - lo_.y;
    const double extent = std::max({w, h, 1e-9});
    tolerance_ = 1e-7 * std::max(1.0, extent + std::abs(lo_.x) + std::abs(lo_.y));
    const auto g = static_cast<double>(
        std::clamp<std::size_t>(static_cast<std::size_t>(std::sqrt(double(tri_.size()))), 1, 1024));
    cell_ = extent / g;
    nx_ = static_cast<std::size_t>(std::floor(w / cell_)) + 1;
    ny_ = static_cast<std::size_t>(std::floor(h / cell_)) + 1;
    std::vector<std::uint32_t> count(nx_ * ny_ + 1, 0);
    auto cells = [&](std::uint32_t t, auto&& fn) {
      dvec2 a = p_[tri_[t][0]], b = p_[tri_[t][1]], c = p_[tri_[t][2]];
      const dvec2 mn = core::min(core::min(a, b), c);
      const dvec2 mx = core::max(core::max(a, b), c);
      const auto [x0, y0] = cell_of(mn);
      const auto [x1, y1] = cell_of(mx);
      for (std::size_t y = y0; y <= y1; ++y) {
        for (std::size_t x = x0; x <= x1; ++x) fn(y * nx_ + x);
      }
    };
    for (std::uint32_t t = 0; t < tri_.size(); ++t) {
      cells(t, [&](std::size_t c) { ++count[c + 1]; });
    }
    for (std::size_t i = 1; i < count.size(); ++i) count[i] += count[i - 1];
    start_ = count;
    items_.resize(count.back());
    std::vector<std::uint32_t> fill(count.begin(), count.end() - 1);
    for (std::uint32_t t = 0; t < tri_.size(); ++t) {
      cells(t, [&](std::size_t c) { items_[fill[c]++] = t; });
    }
  }

  // Whether a triangle covering `q` is nearer the viewer than `depth`.
  [[nodiscard]] bool hidden(const dvec2& q, double depth) const {
    if (tri_.empty()) return false;
    if (q.x < lo_.x || q.y < lo_.y || q.x > hi_.x || q.y > hi_.y) return false;
    const auto [cx, cy] = cell_of(q);
    const std::size_t c = cy * nx_ + cx;
    for (std::uint32_t i = start_[c]; i < start_[c + 1]; ++i) {
      const auto& t = tri_[items_[i]];
      const dvec2 a = p_[t[0]], b = p_[t[1]], d = p_[t[2]];
      const double area = cross(b - a, d - a);
      const double w0 = cross(d - b, q - b) / area;
      const double w1 = cross(a - d, q - d) / area;
      const double w2 = 1.0 - w0 - w1;
      constexpr double kEdge = -1e-12;
      if (w0 < kEdge || w1 < kEdge || w2 < kEdge) continue;
      const double z = w0 * z_[t[0]] + w1 * z_[t[1]] + w2 * z_[t[2]];
      if (z > depth + tolerance_) return true;
    }
    return false;
  }

 private:
  std::pair<std::size_t, std::size_t> cell_of(const dvec2& q) const {
    auto clampi = [](double v, std::size_t n) {
      if (!(v > 0)) return std::size_t{0};
      const auto i = static_cast<std::size_t>(v);
      return std::min(i, n - 1);
    };
    return {clampi((q.x - lo_.x) / cell_, nx_), clampi((q.y - lo_.y) / cell_, ny_)};
  }

  std::vector<dvec2> p_;
  std::vector<double> z_;
  std::vector<std::array<std::uint32_t, 3>> tri_;
  dvec2 lo_{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
  dvec2 hi_{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
  double cell_ = 1.0;
  double tolerance_ = 1e-9;
  std::size_t nx_ = 1, ny_ = 1;
  std::vector<std::uint32_t> start_;
  std::vector<std::uint32_t> items_;
};

// A line cut into pieces about `step` long (in the view), each classified.
struct Pieces {
  std::vector<dvec2> points;     // the line's points, projected
  std::vector<std::size_t> per;  // pieces per segment
  std::vector<std::uint8_t> cls; // 0 visible, 1 hidden, 2 dropped
};

Pieces classify(const Line3& line, const ViewFrame& frame, const Occlusion& occ, double step) {
  Pieces out;
  out.points.reserve(line.size());
  for (const auto& q : line) out.points.push_back(frame.project(q));
  for (std::size_t i = 0; i + 1 < line.size(); ++i) {
    const double len = core::length(out.points[i + 1] - out.points[i]);
    const auto n = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(len / step)));
    out.per.push_back(n);
    for (std::size_t j = 0; j < n; ++j) {
      const double t = (static_cast<double>(j) + 0.5) / static_cast<double>(n);
      const dvec3 m = line[i] + (line[i + 1] - line[i]) * t;
      out.cls.push_back(occ.hidden(frame.project(m), frame.depth(m)) ? 1 : 0);
    }
  }
  return out;
}

// Runs of pieces of one class, as polylines.
void runs(const Pieces& p, std::uint8_t cls, std::vector<std::vector<dvec2>>& out) {
  std::size_t k = 0;
  std::vector<dvec2> current;
  for (std::size_t i = 0; i < p.per.size(); ++i) {
    const dvec2 a = p.points[i];
    const dvec2 b = p.points[i + 1];
    const std::size_t n = p.per[i];
    for (std::size_t j = 0; j < n; ++j, ++k) {
      const dvec2 s = a + (b - a) * (static_cast<double>(j) / static_cast<double>(n));
      const dvec2 e = a + (b - a) * (static_cast<double>(j + 1) / static_cast<double>(n));
      if (p.cls[k] == cls) {
        if (current.empty()) current.push_back(s);
        // Only a segment's own points are kept: pieces in between are on it.
        if (j + 1 == n) current.push_back(e);
      } else if (!current.empty()) {
        if (!(current.back() == s)) current.push_back(s);
        out.push_back(std::move(current));
        current.clear();
      }
    }
  }
  if (current.size() >= 2) out.push_back(std::move(current));
}

double segment_distance(const dvec2& q, const dvec2& a, const dvec2& b) {
  const dvec2 ab = b - a;
  const double l2 = dot(ab, ab);
  double t = l2 > 0 ? dot(q - a, ab) / l2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  return core::length(q - (a + ab * t));
}

// Douglas-Peucker.
std::vector<dvec2> simplify(const std::vector<dvec2>& line, double tolerance) {
  if (line.size() <= 2 || tolerance <= 0) return line;
  std::vector<bool> keep(line.size(), false);
  keep.front() = keep.back() = true;
  std::vector<std::pair<std::size_t, std::size_t>> stack{{0, line.size() - 1}};
  while (!stack.empty()) {
    const auto [i, j] = stack.back();
    stack.pop_back();
    double worst = 0;
    std::size_t at = i;
    for (std::size_t k = i + 1; k < j; ++k) {
      const double d = segment_distance(line[k], line[i], line[j]);
      if (d > worst) {
        worst = d;
        at = k;
      }
    }
    if (worst > tolerance) {
      keep[at] = true;
      stack.push_back({i, at});
      stack.push_back({at, j});
    }
  }
  std::vector<dvec2> out;
  for (std::size_t k = 0; k < line.size(); ++k) {
    if (keep[k]) out.push_back(line[k]);
  }
  // A closed loop simplified to a point keeps two points.
  if (out.size() < 2) out = {line.front(), line.back()};
  return out;
}

// A grid of segments, each tagged with the line it came from, to drop
// hidden pieces lying on lines already drawn.
class SegmentGrid {
 public:
  explicit SegmentGrid(double cell) : cell_(std::max(cell, 1e-9)) {}

  void add_runs(const Pieces& p, std::uint8_t cls, std::size_t tag) {
    std::vector<std::vector<dvec2>> found;
    runs(p, cls, found);
    for (const auto& r : found) {
      for (std::size_t i = 0; i + 1 < r.size(); ++i) add(r[i], r[i + 1], tag);
    }
  }

  // Whether a segment of another line than `skip` passes within
  // `tolerance` of `q`.
  [[nodiscard]] bool near(const dvec2& q, double tolerance, std::size_t skip) const {
    const auto key = cell_key(q);
    for (long dy = -1; dy <= 1; ++dy) {
      for (long dx = -1; dx <= 1; ++dx) {
        const auto it = cells_.find(pack(key.first + dx, key.second + dy));
        if (it == cells_.end()) continue;
        for (const auto s : it->second) {
          if (segs_[s].tag == skip) continue;
          if (segment_distance(q, segs_[s].a, segs_[s].b) <= tolerance) return true;
        }
      }
    }
    return false;
  }

 private:
  struct Seg {
    dvec2 a, b;
    std::size_t tag;
  };
  void add(const dvec2& a, const dvec2& b, std::size_t tag) {
    const auto s = static_cast<std::uint32_t>(segs_.size());
    segs_.push_back({a, b, tag});
    const auto ka = cell_key(a);
    const auto kb = cell_key(b);
    for (long y = std::min(ka.second, kb.second); y <= std::max(ka.second, kb.second); ++y) {
      for (long x = std::min(ka.first, kb.first); x <= std::max(ka.first, kb.first); ++x) {
        cells_[pack(x, y)].push_back(s);
      }
    }
  }
  [[nodiscard]] std::pair<long, long> cell_key(const dvec2& q) const {
    return {static_cast<long>(std::floor(q.x / cell_)), static_cast<long>(std::floor(q.y / cell_))};
  }
  static std::uint64_t pack(long x, long y) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
           static_cast<std::uint32_t>(y);
  }
  double cell_;
  std::vector<Seg> segs_;
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells_;
};

// Marks hidden pieces of line `tag` lying within `tolerance` of the grid's
// segments (of other lines) as dropped.
void drop_near(Pieces& p, const SegmentGrid& grid, double tolerance, std::size_t tag) {
  std::size_t k = 0;
  for (std::size_t i = 0; i < p.per.size(); ++i) {
    const dvec2 a = p.points[i];
    const dvec2 b = p.points[i + 1];
    for (std::size_t j = 0; j < p.per[i]; ++j, ++k) {
      if (p.cls[k] != 1) continue;
      const double t = (static_cast<double>(j) + 0.5) / static_cast<double>(p.per[i]);
      if (grid.near(a + (b - a) * t, tolerance, tag)) p.cls[k] = 2;
    }
  }
}

}  // namespace

ViewProjector::ViewProjector(ViewFrame frame, ProjectionOptions options,
                             core::ProductSettings settings)
    : frame_(frame), options_(std::move(options)), settings_(settings) {}

void ViewProjector::add_outline(const Id& source, const kernel::Shape& shape) {
  items_.push_back({source, false, shape, nullptr});
}

void ViewProjector::add_contents(const Id& source, const kernel::Shape& shape) {
  items_.push_back({source, true, shape, nullptr});
}

void ViewProjector::add_contents(const Id& source, const mesh::Mesh& mesh) {
  items_.push_back({source, true, {}, &mesh});
}

ProjectedView ViewProjector::run(bool hidden) const {
  ProjectedView out;
  struct Source {
    Id id;
    bool contents;
    Extracted extracted;
  };
  std::vector<Source> sources;
  sources.reserve(items_.size());
  for (const auto& item : items_) {
    if (item.mesh) {
      sources.push_back({item.source, true, extract_mesh(*item.mesh, frame_, options_)});
      continue;
    }
    if (item.shape.is_empty()) continue;
    auto e = extract_shape(item.shape, frame_, options_, settings_, item.contents);
    if (!e) {
      out.diagnostics.add(Diagnostic::warning(
          "draw.view.failed",
          fmt::format("couldn't project part of a view: {} ({})", e.error().message,
                      e.error().code),
          {{core::SubjectKind::Volume, item.source, "projected item"}}));
      continue;
    }
    sources.push_back({item.source, item.contents, std::move(*e)});
  }

  auto emit = [&](LineKind kind, const Id& source, const std::vector<dvec2>& points) {
    // Lines seen end-on are points: nothing to draw.
    const bool point = std::all_of(points.begin(), points.end(),
                                   [&](const dvec2& q) { return q == points.front(); });
    if (point) return;
    auto simple = simplify(points, options_.simplify);
    if (simple.size() >= 2) out.lines.push_back({kind, source, std::move(simple)});
  };

  // Outlines, whole.
  for (const auto& s : sources) {
    if (s.contents) continue;
    for (const auto& l : s.extracted.lines) {
      std::vector<dvec2> p;
      p.reserve(l.size());
      for (const auto& q : l) p.push_back(frame_.project(q));
      emit(LineKind::Outline, s.id, p);
    }
  }

  // Contents, with hidden lines removed.
  std::vector<Triangles> sets;
  struct LineRef {
    std::size_t source;
    const Line3* line;
  };
  std::vector<LineRef> lines;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    if (!sources[i].contents) continue;
    sets.push_back(std::move(sources[i].extracted.occluders));
    for (const auto& l : sources[i].extracted.lines) lines.push_back({i, &l});
  }
  if (lines.empty()) return out;
  const Occlusion occ(sets, frame_);
  const double step = std::max(options_.step, 1e-6);
  std::vector<Pieces> pieces(lines.size());
  constexpr std::size_t kChunk = 64;
  const std::size_t chunks = (lines.size() + kChunk - 1) / kChunk;
  auto work = [&](std::size_t c) {
    const std::size_t end = std::min(lines.size(), (c + 1) * kChunk);
    for (std::size_t i = c * kChunk; i < end; ++i) {
      pieces[i] = classify(*lines[i].line, frame_, occ, step);
    }
  };
  if (options_.parallel) {
    options_.parallel(chunks, work);
  } else {
    for (std::size_t c = 0; c < chunks; ++c) work(c);
  }

  if (hidden) {
    // Hidden pieces on a visible line (an edge behind its own face seen
    // edge-on, an edge shared by two touching solids) aren't drawn, nor
    // are hidden pieces on a hidden line drawn before them.
    const double near = std::max(step * 0.25, 2 * options_.deflection);
    SegmentGrid visible(step * 4);
    for (std::size_t i = 0; i < pieces.size(); ++i) visible.add_runs(pieces[i], 0, i);
    const std::size_t none = pieces.size();
    for (auto& p : pieces) drop_near(p, visible, near, none);
    SegmentGrid behind(step * 4);
    for (std::size_t i = 0; i < pieces.size(); ++i) {
      drop_near(pieces[i], behind, near, i);
      behind.add_runs(pieces[i], 1, i);
    }
  }

  for (std::size_t i = 0; i < lines.size(); ++i) {
    const Id& source = sources[lines[i].source].id;
    std::vector<std::vector<dvec2>> visible;
    runs(pieces[i], 0, visible);
    for (const auto& r : visible) emit(LineKind::Visible, source, r);
    if (hidden) {
      std::vector<std::vector<dvec2>> behind;
      runs(pieces[i], 1, behind);
      for (const auto& r : behind) emit(LineKind::Hidden, source, r);
    }
  }
  return out;
}

std::vector<dvec3> shape_points(const kernel::Shape& shape, double deflection,
                                const core::ProductSettings& settings) {
  std::vector<dvec3> out;
  if (shape.is_empty()) return out;
  kernel::TessellationParams params;
  params.deflection = deflection;
  params.angle = 0.2;
  const auto tess = kernel::tessellate(shape, params, settings);
  if (!tess) return out;
  out = tess->vertices;
  for (const auto& e : tess->edges) out.insert(out.end(), e.points.begin(), e.points.end());
  for (const auto& f : tess->faces) out.insert(out.end(), f.points.begin(), f.points.end());
  return out;
}

}  // namespace vs::draw

#include "vs/proc/mesher.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "vs/mesh/topology.hpp"

namespace vs::proc {

using core::Diagnostic;
using core::dvec2;
using core::Id;
using mesh::Triangle;

namespace {

using Index = std::size_t;
using Vec3i = std::array<std::int64_t, 3>;

constexpr std::uint32_t kNone = 0xffffffffu;

bool inside(float v) { return v < 0.0f; }

double double_area(const dvec3& a, const dvec3& b, const dvec3& c) {
  return length(cross(b - a, c - a));
}

// --- the grid's tetrahedra ---------------------------------------------------------------
//
// Corner c of a cube sits at (c & 1, (c >> 1) & 1, (c >> 2) & 1). Each cube
// is split into six tetrahedra round its diagonal from corner 0 to corner 7
// (the Kuhn subdivision): tetrahedron t, for the axis order kOrder[t] =
// (a, b, c), has corners 0, e_a, e_a + e_b and 7. Every cube is split the
// same way, so neighbouring cubes' tetrahedra meet face to face, and the
// grid edges of the subdivision run along seven directions: the axes, the
// face diagonals (1, 1, 0) and so on, and the body diagonal (1, 1, 1).

constexpr std::array<std::array<Index, 3>, 6> kOrder = {
    {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};

constexpr std::array<Index, 4> tet_corners(Index t) {
  const Index a = Index{1} << kOrder[t][0];
  const Index b = Index{1} << kOrder[t][1];
  return {0, a, a | b, 7};
}

constexpr std::int64_t bit(Index corner, Index axis) {
  return static_cast<std::int64_t>((corner >> axis) & 1u);
}

constexpr std::array<Index, 7> kDirections = {1, 2, 4, 3, 6, 5, 7};

// The tetrahedra round the edge from a grid point P to P + direction: each
// as (offset of its cube's lowest corner from P, tetrahedron), in order
// counter-clockwise about the direction.
struct Round {
  std::vector<std::pair<Vec3i, Index>> tets;
};

std::array<Round, 7> make_rounds() {
  std::array<Round, 7> out;
  for (Index di = 0; di < 7; ++di) {
    const Index d = kDirections[di];
    const dvec3 dir = normalize(dvec3{static_cast<double>(bit(d, 0)), static_cast<double>(bit(d, 1)),
                                      static_cast<double>(bit(d, 2))});
    const dvec3 mid = dir * (0.5 * length(dvec3{static_cast<double>(bit(d, 0)),
                                                static_cast<double>(bit(d, 1)),
                                                static_cast<double>(bit(d, 2))}));
    const dvec3 any = std::abs(dir.x) < 0.9 ? dvec3{1, 0, 0} : dvec3{0, 1, 0};
    const dvec3 b1 = normalize(cross(cross(dir, any), dir));
    const dvec3 b2 = cross(dir, b1);
    std::vector<std::pair<double, std::pair<Vec3i, Index>>> found;
    for (Index o = 0; o < 8; ++o) {
      // Cube offset -1 or 0 on each axis; 0 where the edge runs along it.
      const Vec3i offset{-bit(o, 0), -bit(o, 1), -bit(o, 2)};
      bool ok = true;
      Index lu = 0;  // P as a corner of that cube
      for (Index axis = 0; axis < 3; ++axis) {
        if (bit(d, axis) != 0 && offset[axis] != 0) ok = false;
        if (offset[axis] != 0) lu |= Index{1} << axis;
      }
      if (!ok) continue;
      const Index lv = lu | d;  // P + direction (bits don't overlap: checked above)
      for (Index t = 0; t < 6; ++t) {
        const auto c = tet_corners(t);
        if (std::find(c.begin(), c.end(), lu) == c.end()) continue;
        if (std::find(c.begin(), c.end(), lv) == c.end()) continue;
        dvec3 centre{};
        for (const Index k : c) {
          centre = centre + dvec3{static_cast<double>(offset[0] + bit(k, 0)),
                                  static_cast<double>(offset[1] + bit(k, 1)),
                                  static_cast<double>(offset[2] + bit(k, 2))};
        }
        const dvec3 r = centre * 0.25 - mid;
        found.push_back({std::atan2(dot(r, b2), dot(r, b1)), {offset, t}});
      }
    }
    std::sort(found.begin(), found.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    for (const auto& f : found) out[di].tets.push_back(f.second);
  }
  return out;
}

const std::array<Round, 7>& rounds() {
  static const std::array<Round, 7> r = make_rounds();
  return r;
}

// The samples with a layer of "outside" all round, so the surface closes at
// the grid's edge. Padded index p is sample p - 1.
struct Padded {
  const SampledField& s;
  std::int64_t n[3];
  float outside;

  explicit Padded(const SampledField& samples)
      : s(samples),
        n{static_cast<std::int64_t>(samples.grid.nx) + 2,
          static_cast<std::int64_t>(samples.grid.ny) + 2,
          static_cast<std::int64_t>(samples.grid.nz) + 2},
        outside(static_cast<float>(samples.grid.cell)) {}

  [[nodiscard]] float at(std::int64_t i, std::int64_t j, std::int64_t k) const {
    if (i <= 0 || j <= 0 || k <= 0 || i >= n[0] - 1 || j >= n[1] - 1 || k >= n[2] - 1) {
      return outside;
    }
    return s.values[s.grid.index(static_cast<std::uint32_t>(i - 1), static_cast<std::uint32_t>(j - 1),
                                 static_cast<std::uint32_t>(k - 1))];
  }
  [[nodiscard]] dvec3 point(std::int64_t i, std::int64_t j, std::int64_t k) const {
    const GridSpec& g = s.grid;
    return {g.origin.x + static_cast<double>(i - 1) * g.cell,
            g.origin.y + static_cast<double>(j - 1) * g.cell,
            g.origin.z + static_cast<double>(k - 1) * g.cell};
  }
  [[nodiscard]] std::uint64_t cell_key(std::int64_t i, std::int64_t j, std::int64_t k) const {
    return (static_cast<std::uint64_t>(k) * static_cast<std::uint64_t>(n[1] - 1) +
            static_cast<std::uint64_t>(j)) *
               static_cast<std::uint64_t>(n[0] - 1) +
           static_cast<std::uint64_t>(i);
  }
};

// Merges the vertices each cube got (one per tetrahedron) wherever that
// keeps the mesh a closed 2-manifold: collapsing a mesh edge between two
// vertices of the same cube when its link condition holds (the vertices'
// common neighbours are exactly the two triangles' opposite corners) and the
// shell keeps at least four vertices. A cube whose surface is one simple
// disc ends with one vertex, as cube-based surface nets would give it; the
// tangled cases that make cube-based nets non-manifold keep the vertices
// they need. Merged vertices sit at the mean of all their crossing points.
// Vertices and triangles keep their order (compacted), so the result is as
// deterministic as the input.
void merge_cubes(FieldMesh& m, const std::vector<dvec3>& sums, const std::vector<double>& counts,
                 const std::vector<std::uint32_t>& group_starts) {
  const std::size_t nv = m.positions.size();
  std::vector<dvec3> sum = sums;
  std::vector<double> count = counts;
  std::vector<bool> tri_alive(m.triangles.size(), true);
  std::vector<std::vector<std::uint32_t>> fan(nv);
  for (std::uint32_t t = 0; t < m.triangles.size(); ++t) {
    for (const std::uint32_t v : m.triangles[t]) fan[v].push_back(t);
  }
  std::vector<bool> alive(nv, true);

  const auto neighbours = [&](std::uint32_t v, std::vector<std::uint32_t>& out) {
    out.clear();
    for (const std::uint32_t t : fan[v]) {
      for (const std::uint32_t w : m.triangles[t]) {
        if (w != v) out.push_back(w);
      }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  };
  std::vector<std::uint32_t> nu;
  std::vector<std::uint32_t> nw;
  std::vector<std::uint32_t> common;
  std::vector<std::uint32_t> shared;

  // Collapses w into u if that keeps the mesh manifold.
  const auto collapse = [&](std::uint32_t u, std::uint32_t w) {
    shared.clear();
    for (const std::uint32_t t : fan[w]) {
      const Triangle& tri = m.triangles[t];
      if (tri[0] == u || tri[1] == u || tri[2] == u) shared.push_back(t);
    }
    if (shared.size() != 2) return false;  // no edge (or not a manifold one)
    std::array<std::uint32_t, 2> opposite{};
    for (std::size_t s = 0; s < 2; ++s) {
      for (const std::uint32_t x : m.triangles[shared[s]]) {
        if (x != u && x != w) opposite[s] = x;
      }
    }
    if (opposite[0] == opposite[1]) return false;
    neighbours(u, nu);
    neighbours(w, nw);
    common.clear();
    std::set_intersection(nu.begin(), nu.end(), nw.begin(), nw.end(), std::back_inserter(common));
    std::sort(opposite.begin(), opposite.end());
    if (common.size() != 2 || common[0] != opposite[0] || common[1] != opposite[1]) return false;
    if (nu.size() + nw.size() - 2 - 2 < 3) return false;  // would flatten a tetrahedron
    for (const std::uint32_t t : shared) {
      tri_alive[t] = false;
      for (const std::uint32_t x : m.triangles[t]) {
        if (x != w) std::erase(fan[x], t);
      }
    }
    for (const std::uint32_t t : fan[w]) {
      if (!tri_alive[t]) continue;
      for (std::uint32_t& x : m.triangles[t]) {
        if (x == w) x = u;
      }
      fan[u].push_back(t);
    }
    fan[w].clear();
    alive[w] = false;
    sum[u] = sum[u] + sum[w];
    count[u] += count[w];
    return true;
  };

  for (std::size_t c = 0; c + 1 < group_starts.size(); ++c) {
    const std::uint32_t first = group_starts[c];
    const std::uint32_t end = group_starts[c + 1];
    for (bool changed = true; changed;) {
      changed = false;
      for (std::uint32_t u = first; u < end; ++u) {
        if (!alive[u]) continue;
        for (std::uint32_t w = u + 1; w < end; ++w) {
          if (alive[w] && collapse(u, w)) changed = true;
        }
      }
    }
  }

  // Compact, in order.
  std::vector<std::uint32_t> index(nv, kNone);
  std::vector<dvec3> positions;
  for (std::uint32_t v = 0; v < nv; ++v) {
    if (!alive[v] || fan[v].empty()) continue;
    index[v] = static_cast<std::uint32_t>(positions.size());
    positions.push_back(sum[v] * (1.0 / count[v]));
  }
  std::vector<Triangle> triangles;
  for (std::size_t t = 0; t < m.triangles.size(); ++t) {
    if (!tri_alive[t]) continue;
    const Triangle& tri = m.triangles[t];
    triangles.push_back({index[tri[0]], index[tri[1]], index[tri[2]]});
  }
  m.positions = std::move(positions);
  m.triangles = std::move(triangles);
}

}  // namespace

// --- surface nets ----------------------------------------------------------------------

FieldMesh surface_nets(const SampledField& samples) {
  FieldMesh out;
  if (samples.grid.size() == 0 || samples.values.size() != samples.grid.size()) return out;
  const Padded g(samples);

  // Vertices: one per tetrahedron the surface crosses, at the mean of where
  // it crosses the tetrahedron's edges.
  std::unordered_map<std::uint64_t, std::array<std::uint32_t, 6>> cells;
  std::vector<dvec3> sums;    // per vertex: the crossing points it averages
  std::vector<double> counts;
  std::vector<std::uint32_t> group_starts;  // each crossed cube's first vertex
  for (std::int64_t k = 0; k + 1 < g.n[2]; ++k) {
    for (std::int64_t j = 0; j + 1 < g.n[1]; ++j) {
      for (std::int64_t i = 0; i + 1 < g.n[0]; ++i) {
        std::array<float, 8> v{};
        unsigned mask = 0;
        for (Index c = 0; c < 8; ++c) {
          v[c] = g.at(i + bit(c, 0), j + bit(c, 1), k + bit(c, 2));
          if (inside(v[c])) mask |= 1u << c;
        }
        if (mask == 0 || mask == 0xffu) continue;
        const auto in = [&](Index c) { return ((mask >> c) & 1u) != 0; };
        std::array<std::uint32_t, 6> ids{};
        ids.fill(kNone);
        for (Index t = 0; t < 6; ++t) {
          const auto c = tet_corners(t);
          dvec3 sum{};
          double n = 0.0;
          for (Index p = 0; p < 4; ++p) {
            for (Index q = p + 1; q < 4; ++q) {
              const Index lo = c[p];  // corners run 0, e_a, e_a + e_b, 7: lo below hi
              const Index hi = c[q];
              if (in(lo) == in(hi)) continue;
              const dvec3 pa = g.point(i + bit(lo, 0), j + bit(lo, 1), k + bit(lo, 2));
              const dvec3 pb = g.point(i + bit(hi, 0), j + bit(hi, 1), k + bit(hi, 2));
              const double va = v[lo];
              const double vb = v[hi];
              sum = sum + (pa + (pb - pa) * (va / (va - vb)));
              n += 1.0;
            }
          }
          if (n == 0.0) continue;
          if (t == 0 || std::all_of(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(t),
                                    [](std::uint32_t id) { return id == kNone; })) {
            group_starts.push_back(static_cast<std::uint32_t>(out.positions.size()));
          }
          ids[t] = static_cast<std::uint32_t>(out.positions.size());
          out.positions.push_back(sum * (1.0 / n));
          sums.push_back(sum);
          counts.push_back(n);
        }
        cells.emplace(g.cell_key(i, j, k), ids);
      }
    }
  }

  // Polygons: one per crossed edge, from the tetrahedra round it, as a fan
  // from the corner that keeps its smallest triangle largest.
  std::vector<std::uint32_t> polygon;
  for (std::int64_t k = 0; k < g.n[2]; ++k) {
    for (std::int64_t j = 0; j < g.n[1]; ++j) {
      for (std::int64_t i = 0; i < g.n[0]; ++i) {
        const Vec3i p{i, j, k};
        const bool in0 = inside(g.at(i, j, k));
        for (Index di = 0; di < 7; ++di) {
          const Index d = kDirections[di];
          const Vec3i q{i + bit(d, 0), j + bit(d, 1), k + bit(d, 2)};
          if (q[0] >= g.n[0] || q[1] >= g.n[1] || q[2] >= g.n[2]) continue;
          if (inside(g.at(q[0], q[1], q[2])) == in0) continue;
          polygon.clear();
          for (const auto& [offset, t] : rounds()[di].tets) {
            const Vec3i m{p[0] + offset[0], p[1] + offset[1], p[2] + offset[2]};
            bool ok = true;
            for (Index axis = 0; axis < 3; ++axis) {
              if (m[axis] < 0 || m[axis] + 1 >= g.n[axis]) ok = false;
            }
            const auto it = ok ? cells.find(g.cell_key(m[0], m[1], m[2])) : cells.end();
            if (it == cells.end() || it->second[t] == kNone) break;  // can't happen
            polygon.push_back(it->second[t]);
          }
          if (polygon.size() != rounds()[di].tets.size() || polygon.size() < 3) continue;
          if (!in0) std::reverse(polygon.begin(), polygon.end());  // inside at the far end
          const Index n = polygon.size();
          const auto& P = out.positions;
          Index apex = 0;
          double best = -1.0;
          for (Index a = 0; a < n; ++a) {
            double worst = std::numeric_limits<double>::infinity();
            for (Index f = 1; f + 1 < n; ++f) {
              worst = std::min(worst, double_area(P[polygon[a]], P[polygon[(a + f) % n]],
                                                  P[polygon[(a + f + 1) % n]]));
            }
            if (worst > best) {
              best = worst;
              apex = a;
            }
          }
          for (Index f = 1; f + 1 < n; ++f) {
            out.triangles.push_back(
                {polygon[apex], polygon[(apex + f) % n], polygon[(apex + f + 1) % n]});
          }
        }
      }
    }
  }
  group_starts.push_back(static_cast<std::uint32_t>(out.positions.size()));
  merge_cubes(out, sums, counts, group_starts);
  return out;
}

mesh::Mesh to_mesh(const FieldMesh& field_mesh, const Id& volume) {
  mesh::Mesh m;
  m.positions.reserve(field_mesh.positions.size());
  for (const dvec3& p : field_mesh.positions) m.add_vertex(core::to_fp32(p));
  for (const Triangle& t : field_mesh.triangles) m.add_triangle(t, volume);
  return m;
}

SampledField clip_to_boundary(const SampledField& samples, const TriangleDistance& boundary,
                              double inset, eval::ThreadPool* pool) {
  SampledField out = samples;
  const GridSpec& grid = samples.grid;
  const auto slice = [&](std::size_t k) {
    const auto kk = static_cast<std::uint32_t>(k);
    for (std::uint32_t j = 0; j < grid.ny; ++j) {
      for (std::uint32_t i = 0; i < grid.nx; ++i) {
        float& v = out.values[grid.index(i, j, kk)];
        v = std::max(v, static_cast<float>(boundary.signed_distance(grid.point(i, j, kk)) + inset));
      }
    }
  };
  if (pool != nullptr) {
    pool->run(grid.nz, slice);
  } else {
    for (std::size_t k = 0; k < grid.nz; ++k) slice(k);
  }
  return out;
}

// --- volume contents -------------------------------------------------------------------

namespace {

std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
  return (static_cast<std::uint64_t>(a) << 32) | b;
}

bool point_in_polygon(const std::vector<dvec2>& poly, const dvec2& p) {
  bool in = false;
  for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const dvec2& a = poly[i];
    const dvec2& b = poly[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
  }
  return in;
}

double segment_distance(const dvec2& p, const dvec2& a, const dvec2& b) {
  const dvec2 ab = b - a;
  const double len2 = dot(ab, ab);
  double t = len2 > 0.0 ? dot(p - a, ab) / len2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  return length(p - (a + ab * t));
}

// Distance from p to a closed polyline.
double loop_distance(const std::vector<dvec2>& loop, const dvec2& p) {
  double best = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < loop.size(); ++i) {
    best = std::min(best, segment_distance(p, loop[i], loop[(i + 1) % loop.size()]));
  }
  return best;
}

double signed_area(const std::vector<dvec2>& loop) {
  double a = 0.0;
  for (std::size_t i = 0; i < loop.size(); ++i) a += cross(loop[i], loop[(i + 1) % loop.size()]);
  return 0.5 * a;
}

// The mesh while seams are stitched into it.
class Builder {
 public:
  enum class State : std::uint8_t { Field, Seam, Deleted };

  Builder(FieldMesh fm, const TriangleDistance& boundary, double cell)
      : boundary_(boundary), cell_(cell) {
    positions_ = std::move(fm.positions);
    seam_ids_.assign(positions_.size(), Id::null());
    triangles_ = std::move(fm.triangles);
    state_.assign(triangles_.size(), State::Field);
  }

  std::vector<dvec3>& positions() { return positions_; }

  // Stitches `seam` in, or says why it can't.
  std::optional<std::string> stitch(const mesh::Seam& seam) {
    if (seam.triangles.empty() || seam.loops.empty()) return std::string("it has no triangles");
    const dvec3 fn = seam.normal();
    const dvec3 o = seam.frame.origin;
    const dvec3 xa = seam.frame.x_axis;
    const dvec3 ya = seam.frame.y_axis;
    const auto uv = [&](const dvec3& p) { return dvec2{dot(p - o, xa), dot(p - o, ya)}; };

    // Which way is out of the volume, across the patch.
    const Triangle& t0 = seam.triangles.front();
    const dvec3 c = (seam.vertices[t0[0]].position + seam.vertices[t0[1]].position +
                     seam.vertices[t0[2]].position) *
                    (1.0 / 3.0);
    constexpr double kProbe = 1e-3;
    const dvec3 out_n = boundary_.signed_distance(c + fn * kProbe) >=
                                boundary_.signed_distance(c - fn * kProbe)
                            ? fn
                            : fn * -1.0;
    const double turn = dot(out_n, fn) > 0 ? 1.0 : -1.0;

    std::vector<dvec2> outline;
    for (const std::uint32_t v : seam.loops[0].vertices) outline.push_back(seam.vertices[v].uv);
    if (outline.size() < 3) return std::string("its outline is degenerate");

    // 1. The grid's triangles on the patch: within a cell of its plane,
    //    touching the outline or within a cell of it, and facing out. Near
    //    the outline, sideways too: where the patch runs to an edge of the
    //    boundary, the grid's surface rounds the edge off, and taking that
    //    rounding as well leaves the rim on the side wall, below the
    //    outline, so the strip continues the wall instead of notching it.
    const double below = cell_;           // on the patch
    const double below_rim = 2.0 * cell_;  // near its outline: the rounding is deeper
    const double above = 0.25 * cell_;
    const double margin = cell_;
    std::vector<bool> candidate(triangles_.size(), false);
    for (std::size_t t = 0; t < triangles_.size(); ++t) {
      if (state_[t] != State::Field) continue;
      const Triangle& tri = triangles_[t];
      bool within = false;  // a corner inside the outline
      bool near = false;    // a corner within the margin of it
      double depth = 0.0;   // of its deepest corner
      bool ok = true;
      for (const std::uint32_t v : tri) {
        const double d = dot(positions_[v] - o, out_n);
        if (d > above) ok = false;
        depth = std::max(depth, -d);
        const dvec2 q = uv(positions_[v]);
        if (!within && point_in_polygon(outline, q)) within = true;
        if (!near && loop_distance(outline, q) <= margin) near = true;
      }
      if (!ok || !(within || near) || depth > (near ? below_rim : below)) continue;
      const dvec3 n = cross(positions_[tri[1]] - positions_[tri[0]], positions_[tri[2]] - positions_[tri[0]]);
      const double len = length(n);
      if (!(len > 0.0)) continue;
      const double facing = dot(n, out_n) / len;
      if (facing <= (near ? -0.2 : 0.3)) continue;
      candidate[t] = true;
    }

    // Half-edges of the live triangles.
    std::unordered_map<std::uint64_t, std::uint32_t> owner;
    owner.reserve(triangles_.size() * 3);
    for (std::uint32_t t = 0; t < triangles_.size(); ++t) {
      if (state_[t] == State::Deleted) continue;
      const Triangle& tri = triangles_[t];
      for (std::size_t k = 0; k < 3; ++k) owner.emplace(edge_key(tri[k], tri[(k + 1) % 3]), t);
    }
    const auto across = [&](std::uint32_t a, std::uint32_t b) -> std::optional<std::uint32_t> {
      const auto it = owner.find(edge_key(b, a));
      if (it == owner.end()) return std::nullopt;
      return it->second;
    };

    // 2. The largest connected piece of them (by area seen along the normal).
    std::vector<std::int32_t> component(triangles_.size(), -1);
    std::vector<std::vector<std::uint32_t>> pieces;
    std::vector<double> areas;
    for (std::uint32_t t = 0; t < triangles_.size(); ++t) {
      if (!candidate[t] || component[t] >= 0) continue;
      const auto id = static_cast<std::int32_t>(pieces.size());
      pieces.emplace_back();
      areas.push_back(0.0);
      std::vector<std::uint32_t> queue{t};
      component[t] = id;
      for (std::size_t q = 0; q < queue.size(); ++q) {
        const std::uint32_t u = queue[q];
        pieces.back().push_back(u);
        const Triangle& tri = triangles_[u];
        areas.back() += 0.5 * turn *
                        cross(uv(positions_[tri[1]]) - uv(positions_[tri[0]]),
                              uv(positions_[tri[2]]) - uv(positions_[tri[0]]));
        for (std::size_t k = 0; k < 3; ++k) {
          const auto w = across(tri[k], tri[(k + 1) % 3]);
          if (w && candidate[*w] && component[*w] < 0) {
            component[*w] = id;
            queue.push_back(*w);
          }
        }
      }
    }
    if (pieces.empty()) return std::string("the contents don't reach it");
    std::size_t best = 0;
    for (std::size_t p = 1; p < pieces.size(); ++p) {
      if (areas[p] > areas[best]) best = p;
    }
    if (!(areas[best] > 0.0)) return std::string("the contents don't reach it");
    const auto chosen = static_cast<std::int32_t>(best);

    // 3. Its rim: edges of chosen triangles whose neighbour stays. Where it
    //    touches itself at a vertex (round a corner of the patch, say), the
    //    triangles round that vertex join it. Then it must be one simple
    //    loop, or the contents leave gaps in the patch.
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> fans;
    for (std::uint32_t t = 0; t < triangles_.size(); ++t) {
      if (state_[t] != State::Field) continue;
      for (const std::uint32_t v : triangles_[t]) fans[v].push_back(t);
    }
    std::unordered_map<std::uint32_t, std::uint32_t> next;
    std::uint32_t start = 0;
    std::size_t rim_edges = 0;
    for (int attempt = 0;; ++attempt) {
      next.clear();
      rim_edges = 0;
      std::vector<std::uint32_t> pinched;
      for (const std::uint32_t t : pieces[best]) {
        const Triangle& tri = triangles_[t];
        for (std::size_t k = 0; k < 3; ++k) {
          const std::uint32_t a = tri[k];
          const std::uint32_t b = tri[(k + 1) % 3];
          const auto w = across(a, b);
          if (w && component[*w] == chosen) continue;
          if (!next.emplace(a, b).second) pinched.push_back(a);
          if (rim_edges++ == 0) start = a;
        }
      }
      if (pinched.empty()) break;
      if (attempt == 8) return std::string("the contents pinch the patch");
      std::sort(pinched.begin(), pinched.end());
      pinched.erase(std::unique(pinched.begin(), pinched.end()), pinched.end());
      for (const std::uint32_t v : pinched) {
        for (const std::uint32_t t : fans[v]) {
          if (component[t] == chosen) continue;
          component[t] = chosen;
          pieces[best].push_back(t);
        }
      }
    }
    std::vector<std::uint32_t> rim;
    for (std::uint32_t v = start;;) {
      rim.push_back(v);
      const auto it = next.find(v);
      if (it == next.end()) return std::string("the contents leave gaps in the patch");
      v = it->second;
      if (v == start) break;
      if (rim.size() > rim_edges) return std::string("the contents leave gaps in the patch");
    }
    if (rim.size() != rim_edges || rim.size() < 3) {
      return std::string("the contents leave gaps in the patch");
    }

    // The seam's face, facing out: its triangles and every hole fill (the
    // contents have no bolt holes), and its outer loop in the face's own
    // direction.
    std::vector<Triangle> face = seam.triangles_facing(out_n);
    for (const auto& fill : seam.hole_fills) {
      for (Triangle t : fill) {
        if (turn < 0) std::swap(t[1], t[2]);
        face.push_back(t);
      }
    }
    std::vector<std::uint32_t> loop;
    {
      std::unordered_map<std::uint64_t, int> directed;
      for (const Triangle& t : face) {
        for (std::size_t k = 0; k < 3; ++k) directed[edge_key(t[k], t[(k + 1) % 3])] += 1;
      }
      std::unordered_map<std::uint32_t, std::uint32_t> seam_next;
      for (const Triangle& t : face) {
        for (std::size_t k = 0; k < 3; ++k) {
          const std::uint32_t a = t[k];
          const std::uint32_t b = t[(k + 1) % 3];
          if (directed.count(edge_key(b, a)) == 0) seam_next.emplace(a, b);
        }
      }
      const std::uint32_t s0 = seam.loops[0].vertices.front();
      for (std::uint32_t v = s0;;) {
        loop.push_back(v);
        const auto it = seam_next.find(v);
        if (it == seam_next.end() || loop.size() > seam_next.size()) {
          return std::string("its triangulation has no single outer loop");
        }
        v = it->second;
        if (v == s0) break;
      }
      if (loop.size() != seam_next.size()) return std::string("its triangulation has no single outer loop");
    }

    // 4. The rim must follow the outline, both going the same way round.
    std::vector<dvec2> rim2;
    for (const std::uint32_t v : rim) rim2.push_back(uv(positions_[v]));
    std::vector<dvec2> loop2;
    for (const std::uint32_t v : loop) loop2.push_back(seam.vertices[v].uv);
    if (!(signed_area(rim2) * turn > 0.0) || !(signed_area(loop2) * turn > 0.0)) {
      return std::string("the contents fold over it");
    }
    const double track = 3.0 * cell_;
    for (const dvec2& p : loop2) {
      if (loop_distance(rim2, p) > track) return std::string("the contents don't cover all of it");
    }
    for (const dvec2& p : rim2) {
      if (loop_distance(loop2, p) > track) return std::string("the contents don't cover all of it");
    }

    // 5. Replace: drop the chosen triangles, add the seam's face, and join
    //    the rim to the outline with a strip.
    for (const std::uint32_t t : pieces[best]) state_[t] = State::Deleted;
    std::vector<std::uint32_t> map;
    map.reserve(seam.vertices.size());
    for (const mesh::SeamVertex& sv : seam.vertices) {
      map.push_back(static_cast<std::uint32_t>(positions_.size()));
      positions_.push_back(core::to_fp64(sv.position32));
      seam_ids_.push_back(sv.id);
    }
    for (const Triangle& t : face) add({map[t[0]], map[t[1]], map[t[2]]});

    // Start the rim at the vertex nearest the outline's first.
    const dvec3 s_first = core::to_fp64(seam.vertices[loop.front()].position32);
    std::size_t h0 = 0;
    double h0_d = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < rim.size(); ++i) {
      const double d = length_squared(positions_[rim[i]] - s_first);
      if (d < h0_d) {
        h0_d = d;
        h0 = i;
      }
    }
    std::rotate(rim.begin(), rim.begin() + static_cast<std::ptrdiff_t>(h0), rim.end());
    const auto params = [](const std::vector<dvec3>& pts) {
      std::vector<double> u(pts.size() + 1, 0.0);
      for (std::size_t i = 0; i < pts.size(); ++i) {
        u[i + 1] = u[i] + length(pts[(i + 1) % pts.size()] - pts[i]);
      }
      const double total = u.back();
      for (double& x : u) x = total > 0 ? x / total : 0.0;
      return u;
    };
    std::vector<dvec3> rim3;
    for (const std::uint32_t v : rim) rim3.push_back(positions_[v]);
    std::vector<dvec3> loop3;
    for (const std::uint32_t v : loop) loop3.push_back(core::to_fp64(seam.vertices[v].position32));
    const std::vector<double> uh = params(rim3);
    const std::vector<double> us = params(loop3);
    const std::size_t n = rim.size();
    const std::size_t m = loop.size();
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n || j < m) {
      const bool advance_rim = j == m || (i < n && uh[i + 1] <= us[j + 1]);
      if (advance_rim) {
        add({rim[i], rim[(i + 1) % n], map[loop[j % m]]});
        ++i;
      } else {
        add({map[loop[(j + 1) % m]], map[loop[j]], rim[i % n]});
        ++j;
      }
    }
    return std::nullopt;
  }

  // Moves vertices (never seam vertices) inward, against the boundary's
  // gradient, until every vertex and every triangle's centroid (from the
  // FP32 positions) is `inset` inside the boundary, or on it for triangles
  // touching a seam. Returns how many vertices moved.
  std::size_t contain(double inset, eval::ThreadPool* pool) {
    const std::size_t nv = positions_.size();
    const std::size_t nt = triangles_.size();
    // Live triangles round each vertex.
    std::vector<std::uint32_t> first(nv + 1, 0);
    for (std::size_t t = 0; t < nt; ++t) {
      if (state_[t] == State::Deleted) continue;
      for (const std::uint32_t v : triangles_[t]) ++first[v + 1];
    }
    for (std::size_t v = 0; v < nv; ++v) first[v + 1] += first[v];
    std::vector<std::uint32_t> fan(first[nv]);
    {
      std::vector<std::uint32_t> fill(first.begin(), first.end() - 1);
      for (std::size_t t = 0; t < nt; ++t) {
        if (state_[t] == State::Deleted) continue;
        for (const std::uint32_t v : triangles_[t]) fan[fill[v]++] = static_cast<std::uint32_t>(t);
      }
    }
    const auto movable = [&](std::uint32_t v) { return seam_ids_[v].is_null(); };
    const auto rounded = [&](std::uint32_t v) { return core::to_fp64(core::to_fp32(positions_[v])); };
    constexpr double kSlack = 1e-9;
    // How far past its limit a vertex or a triangle's centroid is (<= 0: fine).
    const auto vertex_excess = [&](std::uint32_t v) {
      return movable(v) ? boundary_.signed_distance(rounded(v)) + inset : 0.0;
    };
    const auto triangle_excess = [&](std::uint32_t t) {
      const Triangle& tri = triangles_[t];
      int free = 0;
      for (const std::uint32_t v : tri) free += movable(v) ? 1 : 0;
      if (free == 0) return 0.0;
      const double limit = free == 3 ? inset : 0.0;
      const dvec3 c = (rounded(tri[0]) + rounded(tri[1]) + rounded(tri[2])) * (1.0 / 3.0);
      return (boundary_.signed_distance(c) + limit) * 3.0 / free;
    };

    std::vector<std::uint32_t> vertices;
    for (std::uint32_t v = 0; v < nv; ++v) {
      if (first[v + 1] > first[v] && movable(v)) vertices.push_back(v);
    }
    std::vector<std::uint32_t> tris;
    for (std::uint32_t t = 0; t < nt; ++t) {
      if (state_[t] != State::Deleted) tris.push_back(t);
    }
    std::vector<double> push(nv, 0.0);
    std::vector<std::uint8_t> moved(nv, 0);
    constexpr int kRounds = 16;
    for (int round = 0; round < kRounds && !(vertices.empty() && tris.empty()); ++round) {
      std::vector<double> ve(vertices.size());
      std::vector<double> te(tris.size());
      const auto measure_v = [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) ve[i] = vertex_excess(vertices[i]);
      };
      const auto measure_t = [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) te[i] = triangle_excess(tris[i]);
      };
      if (pool != nullptr) {
        eval::parallel_for(*pool, vertices.size(), 2048, measure_v);
        eval::parallel_for(*pool, tris.size(), 2048, measure_t);
      } else {
        measure_v(0, vertices.size());
        measure_t(0, tris.size());
      }
      std::vector<std::uint32_t> pushed;
      const auto want = [&](std::uint32_t v, double e) {
        if (!movable(v)) return;
        if (push[v] == 0.0) pushed.push_back(v);
        push[v] = std::max(push[v], e);
      };
      for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (ve[i] > kSlack) want(vertices[i], ve[i]);
      }
      for (std::size_t i = 0; i < tris.size(); ++i) {
        if (te[i] > kSlack) {
          for (const std::uint32_t v : triangles_[tris[i]]) want(v, te[i]);
        }
      }
      if (pushed.empty()) break;
      std::sort(pushed.begin(), pushed.end());
      for (const std::uint32_t v : pushed) {
        positions_[v] = positions_[v] - gradient(positions_[v]) * (push[v] + kSlack);
        push[v] = 0.0;
        moved[v] = 1;
      }
      // Next round: what the moved vertices touch.
      vertices = pushed;
      tris.clear();
      for (const std::uint32_t v : pushed) {
        for (std::uint32_t f = first[v]; f < first[v + 1]; ++f) tris.push_back(fan[f]);
      }
      std::sort(tris.begin(), tris.end());
      tris.erase(std::unique(tris.begin(), tris.end()), tris.end());
    }
    std::size_t count = 0;
    for (const auto m : moved) count += m;
    return count;
  }

  // The finished mesh: FP32, unused vertices dropped.
  mesh::Mesh finish(const Id& volume) const {
    mesh::Mesh m;
    std::vector<std::uint32_t> index(positions_.size(), 0xffffffffu);
    std::vector<bool> used(positions_.size(), false);
    for (std::size_t t = 0; t < triangles_.size(); ++t) {
      if (state_[t] == State::Deleted) continue;
      for (const std::uint32_t v : triangles_[t]) used[v] = true;
    }
    for (std::size_t v = 0; v < positions_.size(); ++v) {
      if (used[v]) index[v] = m.add_vertex(core::to_fp32(positions_[v]), seam_ids_[v]);
    }
    for (std::size_t t = 0; t < triangles_.size(); ++t) {
      if (state_[t] == State::Deleted) continue;
      const Triangle& tri = triangles_[t];
      m.add_triangle(index[tri[0]], index[tri[1]], index[tri[2]], volume);
    }
    return m;
  }

 private:
  // The boundary's outward unit gradient at p (central differences).
  dvec3 gradient(const dvec3& p) const {
    constexpr double h = 1e-4;
    const dvec3 g{boundary_.signed_distance(p + dvec3{h, 0, 0}) - boundary_.signed_distance(p - dvec3{h, 0, 0}),
                  boundary_.signed_distance(p + dvec3{0, h, 0}) - boundary_.signed_distance(p - dvec3{0, h, 0}),
                  boundary_.signed_distance(p + dvec3{0, 0, h}) - boundary_.signed_distance(p - dvec3{0, 0, h})};
    const double len = length(g);
    if (len > 0.0) return g * (1.0 / len);
    const dvec3 away = p - boundary_.nearest(p).point;
    const double d = length(away);
    return d > 0.0 ? away * (1.0 / d) : dvec3{0, 0, 0};
  }

  void add(const Triangle& t) {
    triangles_.push_back(t);
    state_.push_back(State::Seam);
  }

  const TriangleDistance& boundary_;
  double cell_;
  std::vector<dvec3> positions_;
  std::vector<Id> seam_ids_;
  std::vector<Triangle> triangles_;
  std::vector<State> state_;
};

}  // namespace

ContentsMesh mesh_contents(const SampledField& samples, const TriangleDistance& boundary,
                           std::span<const mesh::Seam> seams, const core::SubjectRef& volume,
                           eval::ThreadPool* pool) {
  ContentsMesh out;
  FieldMesh fm = surface_nets(clip_to_boundary(samples, boundary, kContentsInset, pool));
  if (fm.empty()) {
    out.diagnostics.add(Diagnostic::warning(
        "proc.mesh.empty",
        fmt::format("'{}': its contents are empty inside its boundary, so it has no mesh",
                    volume.label),
        {volume}));
    return out;
  }

  Builder builder(std::move(fm), boundary, samples.grid.cell);
  for (const mesh::Seam& seam : seams) {
    if (const auto why = builder.stitch(seam)) {
      out.diagnostics.add(Diagnostic::warning(
          "proc.mesh.seam_uncovered",
          fmt::format("'{}': its contents can't take the seam of '{}' ({}), so the volume won't "
                      "weld there",
                      volume.label, seam.name, *why),
          {seam.subject(), volume}));
    } else {
      out.stitched.push_back(seam.surface);
    }
  }
  out.moved_vertices = builder.contain(kContentsInset, pool);
  out.mesh = builder.finish(volume.id);

  const mesh::TopologyReport topology = mesh::analyse(out.mesh);
  if (!topology.watertight() || !topology.manifold()) {
    out.diagnostics.add(Diagnostic::error(
        "proc.mesh.invalid",
        fmt::format("'{}': the mesh of its contents isn't closed and manifold ({} open, {} "
                    "non-manifold and {} inconsistent edges, {} non-manifold vertices)",
                    volume.label, topology.boundary_edges.size(), topology.non_manifold_edges.size(),
                    topology.inconsistent_edges.size(), topology.non_manifold_vertices.size()),
        {volume}));
  }
  return out;
}

}  // namespace vs::proc

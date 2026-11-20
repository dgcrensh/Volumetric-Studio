// The serial surface nets of M11, kept unchanged as the reference that
// the parallel and GPU versions (L1b) must reproduce exactly. Tests and
// self-checks only.

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <vector>

#include "surface_nets_grid.hpp"
#include "vs/proc/mesher.hpp"

namespace vs::proc {

using mesh::Triangle;
using namespace nets;

namespace {


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
// The triangles round each vertex, in one flat array: each vertex has a
// run with room for its first triangles; a run that outgrows its room moves
// to the end with twice the room. (Millions of small vectors cost more than
// the merging itself.)
class Fans {
 public:
  Fans(std::size_t vertices, const std::vector<Triangle>& triangles)
      : start_(vertices + 1, 0), size_(vertices, 0), room_(vertices, 0) {
    for (const Triangle& tri : triangles) {
      for (const std::uint32_t v : tri) ++room_[v];
    }
    for (std::size_t v = 0; v < vertices; ++v) start_[v + 1] = start_[v] + room_[v];
    data_.resize(start_[vertices]);
    for (std::uint32_t t = 0; t < triangles.size(); ++t) {
      for (const std::uint32_t v : triangles[t]) data_[start_[v] + size_[v]++] = t;
    }
  }
  [[nodiscard]] std::span<const std::uint32_t> operator[](std::uint32_t v) const {
    return {data_.data() + start_[v], size_[v]};
  }
  void push_back(std::uint32_t v, std::uint32_t t) {
    if (size_[v] == room_[v]) {
      const std::size_t from = start_[v];
      start_[v] = data_.size();
      room_[v] = std::max<std::uint32_t>(2 * room_[v], 4);
      data_.resize(data_.size() + room_[v]);
      std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(from), size_[v],
                  data_.begin() + static_cast<std::ptrdiff_t>(start_[v]));
    }
    data_[start_[v] + size_[v]++] = t;
  }
  // Removes t from v's run, keeping the others' order.
  void erase(std::uint32_t v, std::uint32_t t) {
    auto* first = data_.data() + start_[v];
    auto* last = first + size_[v];
    size_[v] = static_cast<std::uint32_t>(std::remove(first, last, t) - first);
  }
  void clear(std::uint32_t v) { size_[v] = 0; }

 private:
  std::vector<std::size_t> start_;
  std::vector<std::uint32_t> size_, room_;
  std::vector<std::uint32_t> data_;
};

void merge_cubes(FieldMesh& m, const std::vector<dvec3>& sums, const std::vector<double>& counts,
                 const std::vector<std::uint32_t>& group_starts) {
  const std::size_t nv = m.positions.size();
  std::vector<dvec3> sum = sums;
  std::vector<double> count = counts;
  std::vector<bool> tri_alive(m.triangles.size(), true);
  Fans fan(nv, m.triangles);
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
  std::vector<std::uint32_t> moved;

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
        if (x != w) fan.erase(x, t);
      }
    }
    moved.assign(fan[w].begin(), fan[w].end());  // push_back may move the runs
    for (const std::uint32_t t : moved) {
      if (!tri_alive[t]) continue;
      for (std::uint32_t& x : m.triangles[t]) {
        if (x == w) x = u;
      }
      fan.push_back(u, t);
    }
    fan.clear(w);
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

FieldMesh surface_nets_reference(const SampledField& samples) {
  FieldMesh out;
  if (samples.grid.size() == 0 || samples.values.size() != samples.grid.size()) return out;
  const Padded g(samples);

  // Vertices: one per tetrahedron the surface crosses, at the mean of where
  // it crosses the tetrahedron's edges.
  // Each crossed cube's six tetrahedron vertices, found through a dense
  // index of the cubes (kNone: not crossed).
  std::vector<std::array<std::uint32_t, 6>> cells;
  std::vector<std::uint32_t> cell_of(static_cast<std::size_t>((g.n[0] - 1) * (g.n[1] - 1) * (g.n[2] - 1)),
                                     kNone);
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
        cell_of[g.cell_key(i, j, k)] = static_cast<std::uint32_t>(cells.size());
        cells.push_back(ids);
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
            const std::uint32_t c = ok ? cell_of[g.cell_key(m[0], m[1], m[2])] : kNone;
            if (c == kNone || cells[c][t] == kNone) break;  // can't happen
            polygon.push_back(cells[c][t]);
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

}  // namespace vs::proc

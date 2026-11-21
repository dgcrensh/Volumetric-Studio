#include "vs/proc/mesher.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

#include "surface_nets_grid.hpp"
#include "vs/mesh/topology.hpp"


namespace vs::proc {

using core::Diagnostic;
using core::dvec2;
using core::Id;
using mesh::Triangle;

using namespace nets;

// --- meshing backends ------------------------------------------------------------------

namespace {

std::mutex& backend_mutex() {
  static std::mutex m;
  return m;
}

std::shared_ptr<MeshingBackend>& backend_slot() {
  static std::shared_ptr<MeshingBackend> b;
  return b;
}

}  // namespace

void set_meshing_backend(std::shared_ptr<MeshingBackend> backend) {
  const std::lock_guard lock(backend_mutex());
  backend_slot() = std::move(backend);
}

std::shared_ptr<MeshingBackend> meshing_backend() {
  const std::lock_guard lock(backend_mutex());
  return backend_slot();
}

const NetTables& net_tables() {
  static const NetTables tables = [] {
    NetTables t;
    for (Index k = 0; k < 6; ++k) {
      const auto c = tet_corners(k);
      for (Index i = 0; i < 4; ++i) t.tet_corners[k][i] = static_cast<std::uint32_t>(c[i]);
    }
    for (Index d = 0; d < 7; ++d) {
      t.directions[d] = static_cast<std::uint32_t>(kDirections[d]);
      for (const auto& [offset, tet] : rounds()[d].tets) {
        t.rounds[d].push_back({{static_cast<std::int32_t>(offset[0]), static_cast<std::int32_t>(offset[1]),
                                static_cast<std::int32_t>(offset[2])},
                               static_cast<std::uint32_t>(tet)});
      }
    }
    return t;
  }();
  return tables;
}

// --- the unmerged net ------------------------------------------------------------------
//
// The serial loops of surface_nets_reference(), split by z: each layer of
// cubes (vertices) or of grid points (polygons) is counted first, then
// filled straight into the net's arrays at its offset, so no array exists
// twice (decision 0028). Every value is computed by the same expressions
// from the same samples; only where it is stored changes.

namespace {

// Which of a cube's six tetrahedra the surface crosses (bit t), from the
// mask of its corners inside: those whose corners aren't all on one side.
unsigned crossed_tets(unsigned mask) {
  unsigned out = 0;
  for (Index t = 0; t < 6; ++t) {
    const auto c = tet_corners(t);
    unsigned in = 0;
    for (const Index k : c) in += (mask >> k) & 1u;
    if (in != 0 && in != 4) out |= 1u << t;
  }
  return out;
}

unsigned cube_mask(const Padded& g, std::int64_t i, std::int64_t j, std::int64_t k, std::array<float, 8>& v) {
  unsigned mask = 0;
  for (Index c = 0; c < 8; ++c) {
    v[c] = g.at(i + bit(c, 0), j + bit(c, 1), k + bit(c, 2));
    if (inside(v[c])) mask |= 1u << c;
  }
  return mask;
}

// One z layer of cubes: how many vertices and crossed cubes it has.
std::pair<std::size_t, std::size_t> count_cube_layer(const Padded& g, std::int64_t k) {
  std::size_t vertices = 0;
  std::size_t cubes = 0;
  std::array<float, 8> v{};
  for (std::int64_t j = 0; j + 1 < g.n[1]; ++j) {
    for (std::int64_t i = 0; i + 1 < g.n[0]; ++i) {
      const unsigned mask = cube_mask(g, i, j, k, v);
      if (mask == 0 || mask == 0xffu) continue;
      ++cubes;
      vertices += static_cast<std::size_t>(std::popcount(crossed_tets(mask)));
    }
  }
  return {vertices, cubes};
}

// One z layer of cubes, written at vertex v0 and crossed cube c0.
void cube_layer(const Padded& g, std::int64_t k, std::uint32_t v0, std::size_t c0, UnmergedNet& out,
                std::vector<std::uint8_t>& tets, std::vector<std::uint32_t>& cell_of) {
  std::uint32_t vertex = v0;
  std::size_t cube = c0;
  std::array<float, 8> v{};
  for (std::int64_t j = 0; j + 1 < g.n[1]; ++j) {
    for (std::int64_t i = 0; i + 1 < g.n[0]; ++i) {
      const unsigned mask = cube_mask(g, i, j, k, v);
      if (mask == 0 || mask == 0xffu) continue;
      const auto in = [&](Index c) { return ((mask >> c) & 1u) != 0; };
      // Every crossed cube has a crossed cube edge, and every cube edge is
      // an edge of one of its tetrahedra: at least one vertex.
      out.group_starts[cube] = vertex;
      for (Index t = 0; t < 6; ++t) {
        const auto c = tet_corners(t);
        dvec3 sum{};
        double n = 0.0;
        for (Index p = 0; p < 4; ++p) {
          for (Index q = p + 1; q < 4; ++q) {
            const Index lo = c[p];
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
        out.sums[vertex] = sum;
        out.counts[vertex] = static_cast<std::uint8_t>(n);
        ++vertex;
      }
      const std::uint64_t key = g.cell_key(i, j, k);
      out.cubes[cube] = key;
      tets[cube] = static_cast<std::uint8_t>(crossed_tets(mask));
      cell_of[key] = static_cast<std::uint32_t>(cube);
      ++cube;
    }
  }
}

// The vertices' positions, sum / count, worked out when asked: the same
// expression the serial code stores per vertex, so the same bits, without
// an array of them as large as the sums (H5: 24 bytes a vertex, about half
// a gigabyte on the 100 mm gyroid).
struct Centres {
  const std::vector<dvec3>& sums;
  const std::vector<std::uint8_t>& counts;
  dvec3 operator[](std::uint32_t v) const { return sums[v] * (1.0 / static_cast<double>(counts[v])); }
};

// Tetrahedron t of crossed cube c's vertex (kNone: not crossed): the
// cube's vertices are its crossed tetrahedra's, in order.
struct CubeVertices {
  const std::vector<std::uint32_t>& group_starts;
  const std::vector<std::uint8_t>& tets;
  std::uint32_t operator()(std::uint32_t c, Index t) const {
    const unsigned m = tets[c];
    if (((m >> t) & 1u) == 0) return kNone;
    return group_starts[c] + static_cast<std::uint32_t>(std::popcount(m & ((1u << t) - 1u)));
  }
};

// The polygon round one crossed edge (the serial loop's lookups), into
// `polygon`; false if it is incomplete (which can't happen) or too small.
bool edge_polygon(const Padded& g, std::int64_t i, std::int64_t j, std::int64_t k, Index di,
                  const std::vector<std::uint32_t>& cell_of, const CubeVertices& vertex_of,
                  std::vector<std::uint32_t>& polygon) {
  const Vec3i p{i, j, k};
  polygon.clear();
  for (const auto& [offset, t] : rounds()[di].tets) {
    const Vec3i m{p[0] + offset[0], p[1] + offset[1], p[2] + offset[2]};
    bool ok = true;
    for (Index axis = 0; axis < 3; ++axis) {
      if (m[axis] < 0 || m[axis] + 1 >= g.n[axis]) ok = false;
    }
    const std::uint32_t c = ok ? cell_of[g.cell_key(m[0], m[1], m[2])] : kNone;
    const std::uint32_t v = c == kNone ? kNone : vertex_of(c, t);
    if (v == kNone) break;  // can't happen
    polygon.push_back(v);
  }
  return polygon.size() == rounds()[di].tets.size() && polygon.size() >= 3;
}

// The fan of a polygon (the serial loop's body for one grid point and
// direction), written at out[0], out[1], ...
void fan_polygon(std::vector<std::uint32_t>& polygon, bool in0, const Centres& P, Triangle* out) {
  if (!in0) std::reverse(polygon.begin(), polygon.end());  // inside at the far end
  const Index n = polygon.size();
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
    *out++ = {polygon[apex], polygon[(apex + f) % n], polygon[(apex + f + 1) % n]};
  }
}

// One z layer of grid points: with `out` null, how many triangles it
// makes; else writes them there.
std::size_t point_layer(const Padded& g, std::int64_t k, const std::vector<std::uint32_t>& cell_of,
                        const CubeVertices& vertex_of, const Centres& P, Triangle* out) {
  std::vector<std::uint32_t> polygon;
  std::size_t made = 0;
  for (std::int64_t j = 0; j < g.n[1]; ++j) {
    for (std::int64_t i = 0; i < g.n[0]; ++i) {
      const bool in0 = inside(g.at(i, j, k));
      for (Index di = 0; di < 7; ++di) {
        const Index d = kDirections[di];
        const Vec3i q{i + bit(d, 0), j + bit(d, 1), k + bit(d, 2)};
        if (q[0] >= g.n[0] || q[1] >= g.n[1] || q[2] >= g.n[2]) continue;
        if (inside(g.at(q[0], q[1], q[2])) == in0) continue;
        if (!edge_polygon(g, i, j, k, di, cell_of, vertex_of, polygon)) continue;
        if (out != nullptr) fan_polygon(polygon, in0, P, out + made);
        made += polygon.size() - 2;
      }
    }
  }
  return made;
}

bool stopped(const eval::CancellationToken* cancel) { return cancel != nullptr && cancel->cancelled(); }

// Runs task(i) for i in [0, n), skipping what's left once `cancel` is
// cancelled.
template <typename Task>
void each(eval::ThreadPool* pool, std::size_t n, const Task& task,
          const eval::CancellationToken* cancel = nullptr) {
  const auto guarded = [&](std::size_t i) {
    if (!stopped(cancel)) task(i);
  };
  if (pool != nullptr && pool->size() > 1) {
    pool->run(n, guarded);
  } else {
    for (std::size_t i = 0; i < n; ++i) guarded(i);
  }
}

}  // namespace

UnmergedNet unmerged_net_cpu(const SampledField& samples, eval::ThreadPool* pool,
                             const eval::CancellationToken* cancel) {
  UnmergedNet out;
  if (samples.grid.size() == 0 || samples.values.size() != samples.grid.size()) return out;
  const Padded g(samples);
  const auto cube_layers = static_cast<std::size_t>(g.n[2] - 1);
  const auto point_layers = static_cast<std::size_t>(g.n[2]);

  // Vertices: each layer counted, then filled at its offsets.
  std::vector<std::size_t> vertex_at(cube_layers + 1, 0);
  std::vector<std::size_t> cube_at(cube_layers + 1, 0);
  each(pool, cube_layers, [&](std::size_t k) {
    std::tie(vertex_at[k + 1], cube_at[k + 1]) = count_cube_layer(g, static_cast<std::int64_t>(k));
  }, cancel);
  if (stopped(cancel)) return {};
  for (std::size_t k = 0; k < cube_layers; ++k) {
    vertex_at[k + 1] += vertex_at[k];
    cube_at[k + 1] += cube_at[k];
  }
  const std::size_t nv = vertex_at.back();
  const std::size_t nc = cube_at.back();
  out.sums.resize(nv);
  out.counts.resize(nv);
  out.group_starts.resize(nc + 1);
  out.cubes.resize(nc);
  std::vector<std::uint8_t> tets(nc);
  std::vector<std::uint32_t> cell_of(
      static_cast<std::size_t>((g.n[0] - 1) * (g.n[1] - 1) * (g.n[2] - 1)), kNone);
  each(pool, cube_layers, [&](std::size_t k) {
    cube_layer(g, static_cast<std::int64_t>(k), static_cast<std::uint32_t>(vertex_at[k]), cube_at[k], out, tets,
               cell_of);
  }, cancel);
  if (stopped(cancel)) return {};
  out.group_starts[nc] = static_cast<std::uint32_t>(nv);

  // Polygons: each layer counted, then filled at its offset.
  const Centres positions{out.sums, out.counts};
  const CubeVertices vertex_of{out.group_starts, tets};
  std::vector<std::size_t> tri_at(point_layers + 1, 0);
  each(pool, point_layers, [&](std::size_t k) {
    tri_at[k + 1] = point_layer(g, static_cast<std::int64_t>(k), cell_of, vertex_of, positions, nullptr);
  }, cancel);
  if (stopped(cancel)) return {};
  for (std::size_t k = 0; k < point_layers; ++k) tri_at[k + 1] += tri_at[k];
  out.triangles.resize(tri_at.back());
  each(pool, point_layers, [&](std::size_t k) {
    point_layer(g, static_cast<std::int64_t>(k), cell_of, vertex_of, positions, out.triangles.data() + tri_at[k]);
  }, cancel);
  if (stopped(cancel)) return {};
  return out;
}

UnmergedNet unmerged_net(const SampledField& samples, eval::ThreadPool* pool,
                         const eval::CancellationToken* cancel) {
  if (stopped(cancel)) return {};
  if (samples.grid.size() != 0 && samples.values.size() == samples.grid.size()) {
    if (const auto backend = meshing_backend()) {
      UnmergedNet net;
      if (backend->unmerged_net(samples, net)) return stopped(cancel) ? UnmergedNet{} : net;
    }
  }
  return unmerged_net_cpu(samples, pool, cancel);
}

// --- the merge -------------------------------------------------------------------------
//
// Merges the vertices each cube got (one per tetrahedron) wherever that
// keeps the mesh a closed 2-manifold: collapsing a mesh edge between two
// vertices of the same cube when its link condition holds (the vertices'
// common neighbours are exactly the two triangles' opposite corners) and the
// shell keeps at least four vertices. A cube whose surface is one simple
// disc ends with one vertex, as cube-based surface nets would give it; the
// tangled cases that make cube-based nets non-manifold keep the vertices
// they need. Merged vertices sit at the mean of all their crossing points.
// Vertices and triangles keep their order (compacted).
//
// The same collapses as surface_nets_reference()'s merge_cubes(), cube by
// cube, but in waves (mesher.hpp says why that changes nothing).

namespace {

// Fixed chunks of [0, n), on the pool when there is one; the chunks never
// depend on the thread count.
constexpr std::size_t kChunk = std::size_t{1} << 16;

template <typename Body>
void in_chunks(eval::ThreadPool* pool, std::size_t n, const Body& body) {
  if (pool != nullptr && pool->size() > 1) {
    eval::parallel_for(*pool, n, kChunk, body);
  } else {
    for (std::size_t b = 0; b < n; b += kChunk) body(b, std::min(n, b + kChunk));
  }
}

// Where each chunk's kept items start in the compacted array; the last
// entry is the total.
template <typename Keep>
std::vector<std::size_t> offsets(eval::ThreadPool* pool, std::size_t n, const Keep& keep) {
  const std::size_t chunks = (n + kChunk - 1) / kChunk;
  std::vector<std::size_t> at(chunks + 1, 0);
  in_chunks(pool, n, [&](std::size_t b, std::size_t e) {
    std::size_t c = 0;
    for (std::size_t i = b; i < e; ++i) {
      if (keep(i)) ++c;
    }
    at[b / kChunk + 1] = c;
  });
  for (std::size_t c = 0; c < chunks; ++c) at[c + 1] += at[c];
  return at;
}

// Storage that never moves once handed out, for fans that outgrow their
// room. One per worker; emptied between slabs.
class Arena {
 public:
  std::uint32_t* allocate(std::size_t n) {
    if (used_ + n > size_) {
      size_ = std::max<std::size_t>(kBlock, n);
      blocks_.push_back(std::make_unique<std::uint32_t[]>(size_));
      used_ = 0;
    }
    std::uint32_t* p = blocks_.back().get() + used_;
    used_ += n;
    return p;
  }
  void reset() {
    blocks_.clear();
    used_ = 0;
    size_ = 0;
  }

 private:
  static constexpr std::size_t kBlock = std::size_t{1} << 16;
  std::vector<std::unique_ptr<std::uint32_t[]>> blocks_;
  std::size_t used_ = 0;
  std::size_t size_ = 0;
};

// A dead triangle (collapsed away) is marked in place.
constexpr Triangle kDeadTriangle{kNone, kNone, kNone};
bool dead(const Triangle& t) { return t[0] == kNone; }

// The live triangles round each vertex of a window [base, base + n) of
// vertices, from the triangles [tb, te) (which hold every live triangle
// touching the window): a run per vertex, first in one flat array with
// room for its first triangles; a run that outgrows its room moves to the
// caller's arena with twice the room. Workers growing different vertices'
// runs never touch each other's memory. (Millions of small vectors cost
// more than the merging itself.) The merge never depends on the order of
// a run (it sorts what it reads), so a window's runs may be built afresh
// from the triangles at any time; they are built in increasing order.
class Fans {
 public:
  // A cancelled `cancel` leaves the fans unfinished: the caller drops them.
  Fans(std::uint32_t base, std::size_t vertices, const std::vector<Triangle>& triangles, std::size_t tb,
       std::size_t te, eval::ThreadPool* pool, const eval::CancellationToken* cancel = nullptr)
      : base_(base), run_(vertices, nullptr), size_(vertices, 0), room_(vertices, 0) {
    const auto in = [&](std::uint32_t v) { return v - base_ < size_.size(); };  // dead: kNone, never in
    in_chunks(pool, te - tb, [&](std::size_t b, std::size_t e) {
      if (stopped(cancel)) return;
      for (std::size_t t = tb + b; t < tb + e; ++t) {
        for (const std::uint32_t v : triangles[t]) {
          if (in(v)) std::atomic_ref<std::uint32_t>(room_[v - base_]).fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
    if (stopped(cancel)) return;
    std::size_t total = 0;
    for (const std::uint32_t r : room_) total += r;
    data_.resize(total);
    std::size_t at = 0;
    for (std::size_t v = 0; v < vertices; ++v) {
      run_[v] = data_.data() + at;
      at += room_[v];
    }
    in_chunks(pool, te - tb, [&](std::size_t b, std::size_t e) {
      if (stopped(cancel)) return;
      for (std::size_t t = tb + b; t < tb + e; ++t) {
        for (const std::uint32_t v : triangles[t]) {
          if (!in(v)) continue;
          const std::uint32_t s =
              std::atomic_ref<std::uint32_t>(size_[v - base_]).fetch_add(1, std::memory_order_relaxed);
          run_[v - base_][s] = static_cast<std::uint32_t>(t);
        }
      }
    });
    if (pool != nullptr && pool->size() > 1 && !stopped(cancel)) {
      in_chunks(pool, vertices, [&](std::size_t b, std::size_t e) {
        if (stopped(cancel)) return;
        for (std::size_t v = b; v < e; ++v) std::sort(run_[v], run_[v] + size_[v]);
      });
    }
  }
  [[nodiscard]] std::span<const std::uint32_t> operator[](std::uint32_t v) const {
    return {run_[v - base_], size_[v - base_]};
  }
  void push_back(std::uint32_t v, std::uint32_t t, Arena& arena) {
    v -= base_;
    if (size_[v] == room_[v]) {
      room_[v] = std::max<std::uint32_t>(2 * room_[v], 4);
      std::uint32_t* to = arena.allocate(room_[v]);
      std::copy_n(run_[v], size_[v], to);
      run_[v] = to;
    }
    run_[v][size_[v]++] = t;
  }
  // Removes t from v's run, keeping the others' order.
  void erase(std::uint32_t v, std::uint32_t t) {
    v -= base_;
    std::uint32_t* first = run_[v];
    std::uint32_t* last = first + size_[v];
    size_[v] = static_cast<std::uint32_t>(std::remove(first, last, t) - first);
  }
  void clear(std::uint32_t v) { size_[v - base_] = 0; }
  [[nodiscard]] std::uint32_t size(std::uint32_t v) const { return size_[v - base_]; }

 private:
  std::uint32_t base_;
  std::vector<std::uint32_t*> run_;
  std::vector<std::uint32_t> size_, room_;
  std::vector<std::uint32_t> data_;
};

// What one worker needs while merging a cube.
struct MergeWorker {
  Arena arena;
  std::vector<std::uint32_t> nu, nw, common, shared, moved, merged;
  // nu holds the neighbours of this vertex (kNone: of none). After a
  // collapse of x into u, N(u) is N(u) and N(x) without u and x: the
  // edges x-o of the two removed triangles have other triangles, which
  // become u-o.
  std::uint32_t nu_of = kNone;
};

// The collapses, on the net's own arrays: a vertex is dead once its count
// is 0 (counts are at most 24, four crossings in each of a cube's six
// tetrahedra, so a byte holds them), a triangle once it is kDeadTriangle.
class Merger {
 public:
  Merger(std::vector<Triangle>& triangles, std::vector<dvec3>& sums, std::vector<std::uint8_t>& counts)
      : tri_(triangles), sum_(sums), count_(counts) {}

  void set_fans(Fans* fans) { fan_ = fans; }

  // The serial loop's body for one cube's vertices [first, end).
  void merge_cube(std::uint32_t first, std::uint32_t end, MergeWorker& w) {
    for (bool changed = true; changed;) {
      changed = false;
      w.nu_of = kNone;
      for (std::uint32_t u = first; u < end; ++u) {
        if (count_[u] == 0) continue;
        for (std::uint32_t x = u + 1; x < end; ++x) {
          if (count_[x] != 0 && collapse(u, x, w)) changed = true;
        }
      }
    }
  }

 private:
  void neighbours(std::uint32_t v, std::vector<std::uint32_t>& out) const {
    out.clear();
    for (const std::uint32_t t : (*fan_)[v]) {
      for (const std::uint32_t x : tri_[t]) {
        if (x != v) out.push_back(x);
      }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  }

  // Collapses x into u if that keeps the mesh manifold.
  bool collapse(std::uint32_t u, std::uint32_t x, MergeWorker& w) {
    Fans& fan = *fan_;
    auto& shared = w.shared;
    shared.clear();
    for (const std::uint32_t t : fan[x]) {
      const Triangle& tri = tri_[t];
      if (tri[0] == u || tri[1] == u || tri[2] == u) shared.push_back(t);
    }
    if (shared.size() != 2) return false;  // no edge (or not a manifold one)
    std::array<std::uint32_t, 2> opposite{};
    for (std::size_t s = 0; s < 2; ++s) {
      for (const std::uint32_t y : tri_[shared[s]]) {
        if (y != u && y != x) opposite[s] = y;
      }
    }
    if (opposite[0] == opposite[1]) return false;
    if (w.nu_of != u) {
      neighbours(u, w.nu);
      w.nu_of = u;
    }
    neighbours(x, w.nw);
    w.common.clear();
    std::set_intersection(w.nu.begin(), w.nu.end(), w.nw.begin(), w.nw.end(),
                          std::back_inserter(w.common));
    std::sort(opposite.begin(), opposite.end());
    if (w.common.size() != 2 || w.common[0] != opposite[0] || w.common[1] != opposite[1]) return false;
    if (w.nu.size() + w.nw.size() - 2 - 2 < 3) return false;  // would flatten a tetrahedron
    for (const std::uint32_t t : shared) {
      for (const std::uint32_t y : tri_[t]) {
        if (y != x) fan.erase(y, t);
      }
      tri_[t] = kDeadTriangle;
    }
    w.moved.assign(fan[x].begin(), fan[x].end());
    for (const std::uint32_t t : w.moved) {
      if (dead(tri_[t])) continue;
      for (std::uint32_t& y : tri_[t]) {
        if (y == x) y = u;
      }
      fan.push_back(u, t, w.arena);
    }
    fan.clear(x);
    sum_[u] = sum_[u] + sum_[x];
    count_[u] = static_cast<std::uint8_t>(count_[u] + count_[x]);
    count_[x] = 0;
    w.merged.clear();
    std::set_union(w.nu.begin(), w.nu.end(), w.nw.begin(), w.nw.end(), std::back_inserter(w.merged));
    std::erase_if(w.merged, [&](std::uint32_t y) { return y == u || y == x; });
    std::swap(w.nu, w.merged);
    return true;
  }

  std::vector<Triangle>& tri_;
  std::vector<dvec3>& sum_;
  std::vector<std::uint8_t>& count_;
  Fans* fan_ = nullptr;
};

// Workers' arenas and scratch, handed to each chunk of a wave.
class WorkerPool {
 public:
  MergeWorker* acquire() {
    const std::lock_guard lock(mutex_);
    if (idle_.empty()) {
      workers_.push_back(std::make_unique<MergeWorker>());
      return workers_.back().get();
    }
    MergeWorker* w = idle_.back();
    idle_.pop_back();
    return w;
  }
  void release(MergeWorker* w) {
    const std::lock_guard lock(mutex_);
    idle_.push_back(w);
  }
  // Between slabs: the arenas hold runs of fans that are gone.
  void reset_arenas() {
    for (const auto& w : workers_) w->arena.reset();
  }

 private:
  std::vector<std::unique_ptr<MergeWorker>> workers_;
  std::vector<MergeWorker*> idle_;
  std::mutex mutex_;
};


}  // namespace

// The merge in slabs of whole cube layers, bottom up (decision 0028).
// Collapses in a cube touch only its own vertices and the fans of
// vertices in the cubes round it, and every cube's neighbours below come
// first in the serial order, so running the slabs one after another
// changes nothing; inside a slab, rows of cubes run in waves. A slab
// needs the fans of its own layers and of one layer each side, so those
// are all the fans there ever are; once a slab is done, the layers below
// its last are final, and their vertices and the triangles that use only
// final layers are compacted in place, to the front of the net's own
// arrays.
FieldMesh merge_net(UnmergedNet net, const GridSpec& grid, eval::ThreadPool* pool,
                    const eval::CancellationToken* cancel, std::size_t slab_layers) {
  FieldMesh out;
  if (net.triangles.empty() || net.cubes.empty() || stopped(cancel)) return out;
  const std::uint64_t nx = std::uint64_t{grid.nx} + 1;  // cubes of the padded grid
  const std::uint64_t ny = std::uint64_t{grid.ny} + 1;

  // The layers that have crossed cubes, each with its first cube, and the
  // number of cubes after the last: memory follows the crossings, not the
  // grid's height (H3).
  std::vector<std::uint64_t> layer;
  std::vector<std::size_t> layer_cube;
  for (std::size_t c = 0; c < net.cubes.size(); ++c) {
    const std::uint64_t k = net.cubes[c] / (nx * ny);
    if (layer.empty() || layer.back() != k) {
      layer.push_back(k);
      layer_cube.push_back(c);
    }
  }
  layer_cube.push_back(net.cubes.size());
  // The first crossed cube in layer k or above, and its first vertex.
  const auto cube_from = [&](std::int64_t k) {
    if (k < 0) return std::size_t{0};
    const auto at = std::lower_bound(layer.begin(), layer.end(), static_cast<std::uint64_t>(k));
    return layer_cube[static_cast<std::size_t>(at - layer.begin())];
  };
  const auto vat = [&](std::int64_t k) { return net.group_starts[cube_from(k)]; };

  std::vector<Triangle>& tri = net.triangles;
  std::vector<dvec3>& sum = net.sums;
  std::vector<std::uint8_t>& count = net.counts;
  const std::size_t nt = tri.size();
  const auto top = [&](std::size_t t) { return std::max({tri[t][0], tri[t][1], tri[t][2]}); };
  const auto first = [&](std::size_t c) { return net.group_starts[c]; };
  const auto end = [&](std::size_t c) { return net.group_starts[c + 1]; };

  Merger merger(tri, sum, count);
  WorkerPool workers;
  const bool parallel = pool != nullptr && pool->size() > 1;
  const std::size_t slab = std::max<std::size_t>(slab_layers, 1);

  // Cursors: triangles before `tb` are compacted (to [0, kept_t)), those
  // from `te` on haven't been looked at; vertices before `final_v` are
  // compacted (to [0, kept_v)), and `index` maps [index_base, final_v) to
  // where they went.
  std::size_t tb = 0;
  std::size_t te = 0;
  std::size_t kept_t = 0;
  std::uint32_t final_v = 0;
  std::uint32_t kept_v = 0;
  std::uint32_t index_base = 0;
  std::vector<std::uint32_t> index;
  std::vector<std::pair<std::uint64_t, std::uint32_t>> rows;  // (wave, first cube)

  for (std::size_t li = 0; li < layer.size();) {
    if (stopped(cancel)) return {};
    // Layers [k0, k1), starting at the next with crossed cubes; `next` is
    // the first crossed layer after them (none: this is the last slab).
    const auto k0 = static_cast<std::int64_t>(layer[li]);
    const std::int64_t k1 = k0 + static_cast<std::int64_t>(slab);
    const std::size_t next_li =
        static_cast<std::size_t>(std::lower_bound(layer.begin(), layer.end(), static_cast<std::uint64_t>(k1)) - layer.begin());
    const bool last = next_li == layer.size();
    const std::int64_t next = last ? k1 : static_cast<std::int64_t>(layer[next_li]);
    // The window: vertex layers [k0 - 1, k1]. Its triangles come from grid
    // point layers [k0 - 1, k1 + 1], whose vertices lie in the layer of
    // the point and the one below: every triangle before the first whose
    // top vertex is in layer k0 - 1 or above, and every triangle from the
    // first whose top vertex is in layer k1 + 2, misses it.
    const std::uint32_t w0 = vat(k0 - 1);
    const std::uint32_t w1 = vat(k1 + 1);
    const std::uint32_t past = vat(k1 + 2);
    while (te < nt && top(te) < past) ++te;
    Fans fans(w0, w1 - w0, tri, tb, te, pool, cancel);
    if (stopped(cancel)) return {};
    merger.set_fans(&fans);

    const std::size_t c0 = layer_cube[li];
    const std::size_t c1 = layer_cube[next_li];
    if (!parallel) {
      MergeWorker& w = *workers.acquire();
      constexpr std::size_t kCheckEvery = 4096;  // cubes between looks at the token
      for (std::size_t c = c0; c < c1; ++c) {
        if ((c - c0) % kCheckEvery == 0 && stopped(cancel)) return {};
        merger.merge_cube(first(c), end(c), w);
      }
      workers.release(&w);
    } else {
      // Whole rows of cubes (along x) are the tasks, each merged in grid
      // order: row (j, k) goes in wave j + 3(k - k0). Rows of one wave are
      // three rows or layers apart, so their cubes touch nothing in
      // common; every row with a cube that the serial order merges first
      // and that a cube of this row touches (within two cubes) is in an
      // earlier wave. (The cube-sized waves i + 3j + 9k of L1b are the
      // same argument a level down; rows keep each task's memory together.)
      rows.clear();
      for (std::size_t c = c0; c < c1; ++c) {
        if (c == c0 || net.cubes[c] / nx != net.cubes[c - 1] / nx) {
          const std::uint64_t row = net.cubes[c] / nx;  // k * ny + j
          rows.push_back({(row % ny) + 3 * (row / ny - static_cast<std::uint64_t>(k0)), static_cast<std::uint32_t>(c)});
        }
      }
      const std::size_t nrows = rows.size();
      std::vector<std::uint32_t> row_end(nrows);
      for (std::size_t r = 0; r < nrows; ++r) {
        row_end[r] = r + 1 < nrows ? rows[r + 1].second : static_cast<std::uint32_t>(c1);
      }
      std::vector<std::uint32_t> by_wave(nrows);
      for (std::size_t r = 0; r < nrows; ++r) by_wave[r] = static_cast<std::uint32_t>(r);
      std::stable_sort(by_wave.begin(), by_wave.end(),
                       [&](std::uint32_t a, std::uint32_t b) { return rows[a].first < rows[b].first; });
      const auto merge_row = [&](std::uint32_t r, MergeWorker& w) {
        for (std::uint32_t c = rows[r].second; c < row_end[r]; ++c) merger.merge_cube(first(c), end(c), w);
      };
      MergeWorker* own = workers.acquire();
      for (std::size_t b = 0; b < nrows;) {
        if (stopped(cancel)) return {};
        std::size_t e = b + 1;
        while (e < nrows && rows[by_wave[e]].first == rows[by_wave[b]].first) ++e;
        if (e - b == 1) {
          merge_row(by_wave[b], *own);
        } else {
          eval::parallel_for(*pool, e - b, 1, [&](std::size_t lo_s, std::size_t hi_s) {
            MergeWorker* w = workers.acquire();
            for (std::size_t s = b + lo_s; s < b + hi_s; ++s) merge_row(by_wave[s], *w);
            workers.release(w);
          });
        }
        b = e;
      }
      workers.release(own);
    }

    // Every cube below layer `next` is merged, so vertex layers up to
    // next - 2 are final (all of them after the last slab), all inside the
    // window (layers k1 .. next - 1 have none): compact them, in order. A
    // position goes to a slot at or before its own, whose sum is already
    // used.
    const std::uint32_t done_v = last ? static_cast<std::uint32_t>(sum.size()) : vat(next - 1);
    for (std::uint32_t v = final_v; v < done_v; ++v) {
      if (count[v] == 0 || fans.size(v) == 0) {
        index.push_back(kNone);
        continue;
      }
      index.push_back(kept_v);
      sum[kept_v++] = sum[v] * (1.0 / static_cast<double>(count[v]));
    }
    final_v = done_v;
    // Then the triangles that use only final vertices, in order, up to the
    // first live one that doesn't: the next slab's window starts there.
    while (tb < te && (dead(tri[tb]) || top(tb) < done_v)) {
      if (!dead(tri[tb])) {
        const Triangle& t = tri[tb];
        tri[kept_t++] = {index[t[0] - index_base], index[t[1] - index_base], index[t[2] - index_base]};
      }
      ++tb;
    }
    if (last) {
      while (tb < nt) {
        if (!dead(tri[tb])) {
          const Triangle& t = tri[tb];
          tri[kept_t++] = {index[t[0] - index_base], index[t[1] - index_base], index[t[2] - index_base]};
        }
        ++tb;
      }
    } else {
      // Triangles still to come have a vertex in layer next - 1 or above,
      // so all of theirs in layer next - 2 or above.
      const std::uint32_t keep_from = std::max(index_base, vat(next - 2));
      index.erase(index.begin(), index.begin() + (keep_from - index_base));
      index_base = keep_from;
    }
    merger.set_fans(nullptr);
    workers.reset_arenas();
    li = next_li;
  }

  // Shrink to what was kept, one array at a time.
  std::vector<std::uint8_t>().swap(net.counts);
  std::vector<std::uint64_t>().swap(net.cubes);
  std::vector<std::uint32_t>().swap(net.group_starts);
  std::vector<std::uint32_t>().swap(index);
  out.triangles = std::vector<Triangle>(tri.begin(), tri.begin() + static_cast<std::ptrdiff_t>(kept_t));
  std::vector<Triangle>().swap(tri);
  out.positions = std::vector<dvec3>(sum.begin(), sum.begin() + kept_v);
  std::vector<dvec3>().swap(sum);
  return out;
}

FieldMesh surface_nets(const SampledField& samples, eval::ThreadPool* pool,
                       const eval::CancellationToken* cancel) {
  return merge_net(unmerged_net(samples, pool, cancel), samples.grid, pool, cancel);
}

mesh::Mesh to_mesh(const FieldMesh& field_mesh, const Id& volume) {
  mesh::Mesh m;
  m.positions.reserve(field_mesh.positions.size());
  for (const dvec3& p : field_mesh.positions) m.add_vertex(core::to_fp32(p));
  for (const Triangle& t : field_mesh.triangles) m.add_triangle(t, volume);
  return m;
}

SampledField clip_to_boundary_cpu(const SampledField& samples, const TriangleDistance& boundary,
                                  double inset, eval::ThreadPool* pool,
                                  const eval::CancellationToken* cancel) {
  if (stopped(cancel)) return {};
  SampledField out = samples;
  const GridSpec& grid = samples.grid;
  const auto slice = [&](std::size_t k) {
    if (stopped(cancel)) return;
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
  if (stopped(cancel)) return {};
  return out;
}

SampledField clip_to_boundary(const SampledField& samples, const TriangleDistance& boundary,
                              double inset, eval::ThreadPool* pool,
                              const eval::CancellationToken* cancel) {
  if (stopped(cancel)) return {};
  if (const auto backend = meshing_backend()) {
    SampledField out;
    out.grid = samples.grid;
    if (backend->clip(samples, boundary, inset, out.values) && out.values.size() == samples.grid.size()) {
      return stopped(cancel) ? SampledField{} : out;
    }
  }
  return clip_to_boundary_cpu(samples, boundary, inset, pool, cancel);
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

// Distance from p to a closed polyline in space.
double polyline_distance(const std::vector<dvec3>& loop, const dvec3& p) {
  double best = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < loop.size(); ++i) {
    const dvec3& a = loop[i];
    const dvec3 ab = loop[(i + 1) % loop.size()] - a;
    const double len2 = dot(ab, ab);
    double t = len2 > 0.0 ? dot(p - a, ab) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    best = std::min(best, length(p - (a + ab * t)));
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

  Builder(FieldMesh fm, const TriangleDistance& boundary, double cell,
          const eval::CancellationToken* cancel = nullptr)
      : boundary_(boundary), cell_(cell), cancel_(cancel) {
    positions_ = std::move(fm.positions);
    seam_ids_.assign(positions_.size(), Id::null());
    triangles_ = std::move(fm.triangles);
    state_.assign(triangles_.size(), State::Field);
  }

  std::vector<dvec3>& positions() { return positions_; }

  // Stitches `seam` in, or says why it can't.
  //
  // Planar seams as in M11. Curved seams (L2) work the same way through the
  // seam's surface: heights along its normal at each point, (u, v) unrolled
  // round the axis. A band's patch is a ring, so its removal leaves two
  // rims, and each is stitched to the seam loop it follows.
  std::optional<std::string> stitch(const mesh::Seam& seam) {
    if (seam.triangles.empty() || seam.loops.empty()) return std::string("it has no triangles");
    const bool curved = seam.curved();
    const dvec3 fn = seam.normal();
    const dvec3 o = seam.frame.origin;
    const dvec3 xa = seam.frame.x_axis;
    const dvec3 ya = seam.frame.y_axis;
    const auto uv = [&](const dvec3& p) {
      return curved ? seam.to_uv(p) : dvec2{dot(p - o, xa), dot(p - o, ya)};
    };
    const auto normal_at = [&](const dvec3& p) { return curved ? seam.normal_at(p) : fn; };
    // A triangle's corners in (u, v), whole across a band's cut.
    const double wrap = curved ? 2.0 * std::numbers::pi * seam.radius : 0.0;
    const auto tri_uv = [&](const Triangle& tri) {
      std::array<dvec2, 3> q{uv(positions_[tri[0]]), uv(positions_[tri[1]]), uv(positions_[tri[2]])};
      if (curved) {
        const double hi = std::max({q[0].x, q[1].x, q[2].x});
        for (dvec2& p : q) {
          if (p.x < hi - 0.5 * wrap) p.x += wrap;
        }
      }
      return q;
    };

    // Which way is out of the volume, across the patch.
    const Triangle& t0 = seam.triangles.front();
    const dvec3 c = (seam.vertices[t0[0]].position + seam.vertices[t0[1]].position +
                     seam.vertices[t0[2]].position) *
                    (1.0 / 3.0);
    constexpr double kProbe = 1e-3;
    const dvec3 cn = normal_at(c);
    const double turn = boundary_.signed_distance(c + cn * kProbe) >=
                                boundary_.signed_distance(c - cn * kProbe)
                            ? 1.0
                            : -1.0;
    const dvec3 out_n = cn * turn;
    // How far a point lies out of the volume across the surface.
    const auto height = [&](const dvec3& p) {
      return curved ? seam.offset(p) * turn : dot(p - o, out_n);
    };
    const auto outward_at = [&](const dvec3& p) { return curved ? normal_at(p) * turn : out_n; };

    // The patch's extent in (u, v): its outline polygon, or a band's two
    // ends (y0 and y1, any u).
    std::vector<dvec2> outline;
    for (const std::uint32_t v : seam.loops[0].vertices) outline.push_back(seam.vertices[v].uv);
    if (outline.size() < 3 && !seam.band) return std::string("its outline is degenerate");
    double y0 = 0.0, y1 = 0.0;
    if (seam.band) {
      y0 = seam.vertices[seam.loops[0].vertices.front()].uv.y;
      y1 = seam.vertices[seam.loops[1].vertices.front()].uv.y;
    }
    const auto within_patch = [&](const dvec2& q) {
      return seam.band ? (y0 <= q.y && q.y <= y1) : point_in_polygon(outline, q);
    };
    const auto outline_distance = [&](const dvec2& q) {
      return seam.band ? std::min(std::abs(q.y - y0), std::abs(q.y - y1))
                       : loop_distance(outline, q);
    };

    // 1. The grid's triangles on the patch: within a cell of its surface,
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
    std::size_t candidates = 0;
    for (std::size_t t = 0; t < triangles_.size(); ++t) {
      if (t % 65536 == 0 && stopped(cancel_)) return std::string("cancelled");
      if (state_[t] != State::Field) continue;
      const Triangle& tri = triangles_[t];
      bool within = false;  // a corner inside the outline
      bool near = false;    // a corner within the margin of it
      double depth = 0.0;   // of its deepest corner
      bool ok = true;
      for (const std::uint32_t v : tri) {
        const double d = height(positions_[v]);
        if (d > above) ok = false;
        depth = std::max(depth, -d);
        const dvec2 q = uv(positions_[v]);
        if (!within && within_patch(q)) within = true;
        if (!near && outline_distance(q) <= margin) near = true;
      }
      if (!ok || !(within || near) || depth > (near ? below_rim : below)) continue;
      const dvec3 n = cross(positions_[tri[1]] - positions_[tri[0]], positions_[tri[2]] - positions_[tri[0]]);
      const double len = length(n);
      if (!(len > 0.0)) continue;
      const dvec3 centre =
          (positions_[tri[0]] + positions_[tri[1]] + positions_[tri[2]]) * (1.0 / 3.0);
      const double facing = dot(n, outward_at(centre)) / len;
      if (facing <= (near ? -0.2 : 0.3)) continue;
      candidate[t] = true;
      ++candidates;
    }

    // Half-edges of the candidates, and later of the triangles that join
    // the chosen piece: the only ones the steps below look across to (H5:
    // every live triangle's took about 1.5 GB on a 10M-triangle gyroid).
    // The mesh is manifold, so each half-edge has one owner either way.
    std::unordered_map<std::uint64_t, std::uint32_t> owner;
    owner.reserve(candidates * 3);
    const auto own = [&](std::uint32_t t) {
      const Triangle& tri = triangles_[t];
      for (std::size_t k = 0; k < 3; ++k) owner.emplace(edge_key(tri[k], tri[(k + 1) % 3]), t);
    };
    for (std::uint32_t t = 0; t < triangles_.size(); ++t) {
      if (candidate[t]) own(t);
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
        if (curved) {
          const std::array<dvec2, 3> p = tri_uv(tri);
          areas.back() += 0.5 * turn * cross(p[1] - p[0], p[2] - p[0]);
        } else {
          areas.back() += 0.5 * turn *
                          cross(uv(positions_[tri[1]]) - uv(positions_[tri[0]]),
                                uv(positions_[tri[2]]) - uv(positions_[tri[0]]));
        }
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

    // 3. Its rims: edges of chosen triangles whose neighbour stays. Where a
    //    rim touches itself at a vertex (round a corner of the patch, say),
    //    the triangles round that vertex join it. Then the rims must be
    //    simple loops, one per loop of the seam's face (a band has two), or
    //    the contents leave gaps in the patch.
    std::unordered_map<std::uint32_t, std::uint32_t> next;
    std::vector<std::uint32_t> starts;  // each rim edge's start, in order
    for (int attempt = 0;; ++attempt) {
      next.clear();
      starts.clear();
      std::vector<std::uint32_t> pinched;
      for (const std::uint32_t t : pieces[best]) {
        const Triangle& tri = triangles_[t];
        for (std::size_t k = 0; k < 3; ++k) {
          const std::uint32_t a = tri[k];
          const std::uint32_t b = tri[(k + 1) % 3];
          const auto w = across(a, b);
          if (w && component[*w] == chosen) continue;
          if (!next.emplace(a, b).second) pinched.push_back(a);
          starts.push_back(a);
        }
      }
      if (pinched.empty()) break;
      if (attempt == 8) return std::string("the contents pinch the patch");
      std::sort(pinched.begin(), pinched.end());
      pinched.erase(std::unique(pinched.begin(), pinched.end()), pinched.end());
      // The field triangles round the pinched vertices, in triangle order
      // (found by one pass, not kept for every vertex: H5).
      std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> fans;
      for (const std::uint32_t v : pinched) fans[v];
      for (std::uint32_t t = 0; t < triangles_.size(); ++t) {
        if (state_[t] != State::Field) continue;
        for (const std::uint32_t v : triangles_[t]) {
          const auto it = fans.find(v);
          if (it != fans.end()) it->second.push_back(t);
        }
      }
      for (const std::uint32_t v : pinched) {
        for (const std::uint32_t t : fans[v]) {
          if (component[t] == chosen) continue;
          component[t] = chosen;
          pieces[best].push_back(t);
          own(t);
        }
      }
    }
    const std::size_t want_loops = seam.band ? 2 : 1;
    std::vector<std::vector<std::uint32_t>> rims;
    {
      std::unordered_set<std::uint32_t> seen;
      std::size_t walked = 0;
      for (const std::uint32_t start : starts) {
        if (seen.contains(start)) continue;
        std::vector<std::uint32_t> rim;
        for (std::uint32_t v = start;;) {
          rim.push_back(v);
          seen.insert(v);
          const auto it = next.find(v);
          if (it == next.end()) return std::string("the contents leave gaps in the patch");
          v = it->second;
          if (v == start) break;
          if (rim.size() > starts.size() || seen.contains(v)) {
            return std::string("the contents leave gaps in the patch");
          }
        }
        walked += rim.size();
        if (rim.size() < 3) return std::string("the contents leave gaps in the patch");
        rims.push_back(std::move(rim));
        if (rims.size() > want_loops) return std::string("the contents leave gaps in the patch");
      }
      if (rims.size() != want_loops || walked != starts.size()) {
        return std::string("the contents leave gaps in the patch");
      }
    }

    // The seam's face, facing out: its triangles and every hole fill (the
    // contents have no bolt holes), and its loops in the face's own
    // direction.
    std::vector<Triangle> face = curved ? seam.oriented_triangles(turn > 0)
                                        : seam.triangles_facing(out_n);
    for (const auto& fill : seam.hole_fills) {
      for (Triangle t : fill) {
        if (turn < 0) std::swap(t[1], t[2]);
        face.push_back(t);
      }
    }
    std::vector<std::vector<std::uint32_t>> loops;
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
      std::size_t walked = 0;
      for (std::size_t l = 0; l < want_loops; ++l) {
        std::vector<std::uint32_t> loop;
        const std::uint32_t s0 = seam.loops[l].vertices.front();
        for (std::uint32_t v = s0;;) {
          loop.push_back(v);
          const auto it = seam_next.find(v);
          if (it == seam_next.end() || loop.size() > seam_next.size()) {
            return std::string("its triangulation has no single outer loop");
          }
          v = it->second;
          if (v == s0) break;
        }
        walked += loop.size();
        loops.push_back(std::move(loop));
      }
      if (walked != seam_next.size()) return std::string("its triangulation has no single outer loop");
    }

    // 4. Each rim must follow its loop of the outline, both going the same
    //    way round.
    const double track = 3.0 * cell_;
    if (!curved) {
      const std::vector<std::uint32_t>& rim = rims[0];
      const std::vector<std::uint32_t>& loop = loops[0];
      std::vector<dvec2> rim2;
      for (const std::uint32_t v : rim) rim2.push_back(uv(positions_[v]));
      std::vector<dvec2> loop2;
      for (const std::uint32_t v : loop) loop2.push_back(seam.vertices[v].uv);
      if (!(signed_area(rim2) * turn > 0.0) || !(signed_area(loop2) * turn > 0.0)) {
        return std::string("the contents fold over it");
      }
      for (const dvec2& p : loop2) {
        if (loop_distance(rim2, p) > track) return std::string("the contents don't cover all of it");
      }
      for (const dvec2& p : rim2) {
        if (loop_distance(loop2, p) > track) return std::string("the contents don't cover all of it");
      }
    } else {
      // On a cylinder: pair rims with loops by height along the axis, check
      // each pair winds the same way in u (a band's loops wind once round
      // the axis; a rectangle's not at all, but its area has the sign), and
      // track each other in space.
      const auto mean_v = [&](const std::vector<dvec3>& pts) {
        double s = 0.0;
        for (const dvec3& p : pts) s += uv(p).y;
        return s / static_cast<double>(pts.size());
      };
      const auto winding = [&](const std::vector<dvec3>& pts) {
        // Signed area in unwrapped (u, v), and the total turn in u.
        double area = 0.0, du = 0.0;
        std::vector<dvec2> q;
        for (const dvec3& p : pts) q.push_back(uv(p));
        for (std::size_t i = 1; i < q.size(); ++i) {
          double d = q[i].x - q[i - 1].x;
          if (d > 0.5 * wrap) d -= wrap;
          if (d < -0.5 * wrap) d += wrap;
          q[i].x = q[i - 1].x + d;
          du += d;
        }
        double d = uv(pts.front()).x - q.back().x;
        d -= wrap * std::round(d / wrap);
        du += d;
        for (std::size_t i = 0; i < q.size(); ++i) area += cross(q[i], q[(i + 1) % q.size()]);
        return std::pair{0.5 * area, du};
      };
      std::vector<std::vector<dvec3>> rim3(rims.size()), loop3(loops.size());
      for (std::size_t r = 0; r < rims.size(); ++r) {
        for (const std::uint32_t v : rims[r]) rim3[r].push_back(positions_[v]);
      }
      for (std::size_t l = 0; l < loops.size(); ++l) {
        for (const std::uint32_t v : loops[l]) loop3[l].push_back(seam.vertices[v].position);
      }
      if (rims.size() == 2 &&
          std::abs(mean_v(rim3[0]) - mean_v(loop3[0])) > std::abs(mean_v(rim3[1]) - mean_v(loop3[0]))) {
        std::swap(rims[0], rims[1]);
        std::swap(rim3[0], rim3[1]);
      }
      for (std::size_t l = 0; l < loops.size(); ++l) {
        const auto [ra, ru] = winding(rim3[l]);
        const auto [la, lu] = winding(loop3[l]);
        if (seam.band) {
          if (!(std::abs(ru) > 0.5 * wrap) || !(ru * lu > 0.0)) {
            return std::string("the contents fold over it");
          }
        } else if (!(ra * turn > 0.0) || !(la * turn > 0.0)) {
          return std::string("the contents fold over it");
        }
        for (const dvec3& p : loop3[l]) {
          if (polyline_distance(rim3[l], p) > track) {
            return std::string("the contents don't cover all of it");
          }
        }
        for (const dvec3& p : rim3[l]) {
          if (polyline_distance(loop3[l], p) > track) {
            return std::string("the contents don't cover all of it");
          }
        }
      }
    }

    // 5. Replace: drop the chosen triangles, add the seam's face, and join
    //    each rim to its loop of the outline with a strip.
    for (const std::uint32_t t : pieces[best]) state_[t] = State::Deleted;
    std::vector<std::uint32_t> map;
    map.reserve(seam.vertices.size());
    for (const mesh::SeamVertex& sv : seam.vertices) {
      map.push_back(static_cast<std::uint32_t>(positions_.size()));
      positions_.push_back(core::to_fp64(sv.position32));
      seam_ids_.push_back(sv.id);
    }
    for (const Triangle& t : face) add({map[t[0]], map[t[1]], map[t[2]]});

    for (std::size_t l = 0; l < loops.size(); ++l) {
      std::vector<std::uint32_t>& rim = rims[l];
      const std::vector<std::uint32_t>& loop = loops[l];
      // Start the rim at the vertex nearest the loop's first.
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
      if (stopped(cancel_)) return 0;  // the caller drops the mesh
      std::vector<double> ve(vertices.size());
      std::vector<double> te(tris.size());
      // A whole round measures every vertex and triangle: chunks check the
      // token, so a cancellation lands within one chunk (H5).
      const auto measure_v = [&](std::size_t b, std::size_t e) {
        if (stopped(cancel_)) return;
        for (std::size_t i = b; i < e; ++i) ve[i] = vertex_excess(vertices[i]);
      };
      const auto measure_t = [&](std::size_t b, std::size_t e) {
        if (stopped(cancel_)) return;
        for (std::size_t i = b; i < e; ++i) te[i] = triangle_excess(tris[i]);
      };
      if (pool != nullptr) {
        eval::parallel_for(*pool, vertices.size(), 2048, measure_v);
        eval::parallel_for(*pool, tris.size(), 2048, measure_t);
      } else {
        for (std::size_t b = 0; b < vertices.size(); b += 2048) measure_v(b, std::min(b + 2048, vertices.size()));
        for (std::size_t b = 0; b < tris.size(); b += 2048) measure_t(b, std::min(b + 2048, tris.size()));
      }
      if (stopped(cancel_)) return 0;
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
  // In chunks on `pool` (L1b), each written at its offset: the same mesh
  // as adding vertices and triangles one by one in order.
  mesh::Mesh finish(const Id& volume, eval::ThreadPool* pool) const {
    mesh::Mesh m;
    const std::size_t nv = positions_.size();
    const std::size_t nt = triangles_.size();
    std::vector<std::uint8_t> used(nv, 0);
    in_chunks(pool, nt, [&](std::size_t b, std::size_t e) {
      for (std::size_t t = b; t < e; ++t) {
        if (state_[t] == State::Deleted) continue;
        for (const std::uint32_t v : triangles_[t]) {
          std::atomic_ref<std::uint8_t>(used[v]).store(1, std::memory_order_relaxed);
        }
      }
    });
    const auto keep_v = [&](std::size_t v) { return used[v] != 0; };
    const std::vector<std::size_t> v_at = offsets(pool, nv, keep_v);
    std::vector<std::uint32_t> index(nv, 0xffffffffu);
    m.positions.resize(v_at.back());
    m.seam_vertices.resize(v_at.back());
    in_chunks(pool, nv, [&](std::size_t b, std::size_t e) {
      std::size_t at = v_at[b / kChunk];
      for (std::size_t v = b; v < e; ++v) {
        if (!keep_v(v)) continue;
        index[v] = static_cast<std::uint32_t>(at);
        m.positions[at] = core::to_fp32(positions_[v]);
        m.seam_vertices[at++] = seam_ids_[v];
      }
    });
    const auto keep_t = [&](std::size_t t) { return state_[t] != State::Deleted; };
    const std::vector<std::size_t> t_at = offsets(pool, nt, keep_t);
    m.indices.resize(3 * t_at.back());
    m.triangle_volumes.resize(t_at.back());
    in_chunks(pool, nt, [&](std::size_t b, std::size_t e) {
      std::size_t at = t_at[b / kChunk];
      for (std::size_t t = b; t < e; ++t) {
        if (!keep_t(t)) continue;
        const Triangle& tri = triangles_[t];
        for (std::size_t c = 0; c < 3; ++c) m.indices[3 * at + c] = index[tri[c]];
        m.triangle_volumes[at++] = volume;
      }
    });
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
  const eval::CancellationToken* cancel_ = nullptr;
  std::vector<dvec3> positions_;
  std::vector<Id> seam_ids_;
  std::vector<Triangle> triangles_;
  std::vector<State> state_;
};

}  // namespace

bool closed_manifold(const mesh::Mesh& m, eval::ThreadPool* pool) {
  const std::size_t nv = m.vertex_count();
  const std::size_t nt = m.triangle_count();
  for (const std::uint32_t i : m.indices) {
    if (i >= nv) return false;
  }
  // Round each vertex v, one entry per triangle (v, b, c): b then c.
  std::vector<std::uint32_t> count(nv + 1, 0);
  in_chunks(pool, nt, [&](std::size_t b, std::size_t e) {
    for (std::size_t t = b; t < e; ++t) {
      for (std::size_t c = 0; c < 3; ++c) {
        std::atomic_ref<std::uint32_t>(count[m.indices[3 * t + c]]).fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  std::vector<std::size_t> start(nv + 1, 0);
  for (std::size_t v = 0; v < nv; ++v) start[v + 1] = start[v] + count[v];
  std::fill(count.begin(), count.end(), 0u);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> round(start[nv]);
  in_chunks(pool, nt, [&](std::size_t b, std::size_t e) {
    for (std::size_t t = b; t < e; ++t) {
      const std::uint32_t* i = &m.indices[3 * t];
      for (std::size_t c = 0; c < 3; ++c) {
        const std::uint32_t v = i[c];
        const std::uint32_t s = std::atomic_ref<std::uint32_t>(count[v]).fetch_add(1, std::memory_order_relaxed);
        round[start[v] + s] = {i[(c + 1) % 3], i[(c + 2) % 3]};
      }
    }
  });
  using Entry = std::pair<std::uint32_t, std::uint32_t>;
  in_chunks(pool, nv, [&](std::size_t b, std::size_t e) {
    for (std::size_t v = b; v < e; ++v) std::sort(round.data() + start[v], round.data() + start[v + 1]);
  });
  // The entry of `at`'s round starting at `from`, if there is one.
  const auto find = [&](std::size_t at, std::uint32_t from) -> const Entry* {
    const Entry* f = round.data() + start[at];
    const Entry* l = round.data() + start[at + 1];
    const Entry* p = std::lower_bound(f, l, Entry{from, 0});
    return p != l && p->first == from ? p : nullptr;
  };
  std::atomic<bool> ok = true;
  in_chunks(pool, nv, [&](std::size_t b, std::size_t e) {
    for (std::size_t v = b; v < e && ok.load(std::memory_order_relaxed); ++v) {
      const Entry* first = round.data() + start[v];
      const Entry* last = round.data() + start[v + 1];
      if (first == last) continue;  // unreferenced
      bool good = true;
      for (const Entry* p = first; p != last && good; ++p) {
        if (p->first == v || p->second == v || p->first == p->second) good = false;  // degenerate
        if (p + 1 != last && p[1].first == p->first) good = false;  // v -> b twice
        if (find(p->first, static_cast<std::uint32_t>(v)) == nullptr) good = false;  // no b -> v
      }
      // The link: after b comes c round v; one cycle through all of them.
      if (good) {
        const std::size_t deg = static_cast<std::size_t>(last - first);
        std::uint32_t at = first->first;
        std::size_t steps = 0;
        do {
          const Entry* p = find(v, at);
          if (p == nullptr) {
            good = false;
            break;
          }
          at = p->second;
          ++steps;
        } while (at != first->first && steps <= deg);
        if (steps != deg) good = false;
      }
      if (!good) ok.store(false, std::memory_order_relaxed);
    }
  });
  return ok.load();
}

ContentsMesh mesh_contents(const SampledField& samples, const TriangleDistance& boundary,
                           std::span<const mesh::Seam> seams, const core::SubjectRef& volume,
                           eval::ThreadPool* pool, const eval::CancellationToken* cancel) {
  ContentsMesh out;
  const auto give_up = [&] {
    ContentsMesh c;
    c.cancelled = true;
    return c;
  };
  FieldMesh fm = surface_nets(clip_to_boundary(samples, boundary, kContentsInset, pool, cancel), pool,
                              cancel);
  if (stopped(cancel)) return give_up();
  if (fm.empty()) {
    out.diagnostics.add(Diagnostic::warning(
        "proc.mesh.empty",
        fmt::format("'{}': its contents are empty inside its boundary, so it has no mesh",
                    volume.label),
        {volume}));
    return out;
  }

  Builder builder(std::move(fm), boundary, samples.grid.cell, cancel);
  for (const mesh::Seam& seam : seams) {
    if (stopped(cancel)) return give_up();
    if (const auto why = builder.stitch(seam)) {
      if (stopped(cancel)) return give_up();
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
  if (stopped(cancel)) return give_up();
  out.moved_vertices = builder.contain(kContentsInset, pool);
  if (stopped(cancel)) return give_up();
  out.mesh = builder.finish(volume.id, pool);
  if (stopped(cancel)) return give_up();

  const bool clean = closed_manifold(out.mesh, pool);
  if (clean) return out;
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

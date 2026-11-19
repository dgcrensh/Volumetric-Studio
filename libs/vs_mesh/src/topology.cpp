#include "vs/mesh/topology.hpp"

#include <algorithm>
#include <numeric>
#include <unordered_map>

namespace vs::mesh {
namespace {

std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
  if (a > b) std::swap(a, b);
  return (static_cast<std::uint64_t>(a) << 32) | b;
}

struct UnionFind {
  std::vector<std::uint32_t> parent;
  explicit UnionFind(std::size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0u); }
  std::uint32_t find(std::uint32_t x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  }
  void unite(std::uint32_t a, std::uint32_t b) {
    a = find(a);
    b = find(b);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);
  }
};

}  // namespace

HalfEdges::HalfEdges(const Mesh& mesh) {
  const std::size_t n = mesh.indices.size();
  from_.assign(mesh.indices.begin(), mesh.indices.end());
  twin_.assign(n, kNone);
  use_.assign(n, 0);
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_edge;
  by_edge.reserve(n);
  for (std::uint32_t h = 0; h < n; ++h) by_edge[edge_key(from(h), to(h))].push_back(h);
  for (const auto& [key, hs] : by_edge) {
    for (const std::uint32_t h : hs) use_[h] = static_cast<std::uint32_t>(hs.size());
    if (hs.size() == 2 && from(hs[0]) == to(hs[1]) && to(hs[0]) == from(hs[1]) &&
        from(hs[0]) != to(hs[0])) {
      twin_[hs[0]] = hs[1];
      twin_[hs[1]] = hs[0];
    }
  }
}

TopologyReport analyse(const Mesh& mesh) {
  // Linear in the mesh's size (M12 meshes run to millions of triangles):
  // half-edges are bucketed by their lower vertex, and edges numbered in
  // order of first use, as before.
  TopologyReport r;
  const std::size_t tri_count = mesh.triangle_count();
  const std::size_t nv = mesh.vertex_count();
  const std::size_t nh = mesh.indices.size();
  r.triangles = tri_count;
  constexpr std::uint32_t kNoEdge = 0xffffffffu;

  const auto ends = [&](std::size_t h) {
    const std::size_t t = h / 3;
    const std::uint32_t a = mesh.indices[h];
    const std::uint32_t b = mesh.indices[3 * t + (h % 3 + 1) % 3];
    return std::pair{a, b};
  };

  std::vector<bool> referenced(nv, false);
  for (std::uint32_t t = 0; t < tri_count; ++t) {
    const Triangle tri = mesh.triangle(t);
    for (const std::uint32_t v : tri) referenced[v] = true;
    if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) {
      r.degenerate_triangles.push_back(t);
    } else if (double_area(mesh, t) == 0.0) {
      ++r.zero_area_triangles;
    }
  }
  r.vertices = static_cast<std::size_t>(std::count(referenced.begin(), referenced.end(), true));

  // Half-edges by lower vertex (in half-edge order within each bucket).
  std::vector<std::uint32_t> bucket_start(nv + 1, 0);
  for (std::size_t h = 0; h < nh; ++h) {
    const auto [a, b] = ends(h);
    if (a != b) ++bucket_start[std::min(a, b) + 1];
  }
  for (std::size_t v = 0; v < nv; ++v) bucket_start[v + 1] += bucket_start[v];
  std::vector<std::uint32_t> bucket(bucket_start[nv]);
  {
    std::vector<std::uint32_t> fill(bucket_start.begin(), bucket_start.end() - 1);
    for (std::size_t h = 0; h < nh; ++h) {
      const auto [a, b] = ends(h);
      if (a != b) bucket[fill[std::min(a, b)]++] = static_cast<std::uint32_t>(h);
    }
  }
  // Each half-edge's edge, edges numbered by first use.
  std::vector<std::uint32_t> edge_of(nh, kNoEdge);
  std::vector<Edge> edges;
  for (std::size_t h = 0; h < nh; ++h) {
    const auto [a, b] = ends(h);
    if (a == b) continue;
    const std::uint32_t lo = std::min(a, b);
    const std::uint32_t hi = std::max(a, b);
    for (std::uint32_t k = bucket_start[lo]; k < bucket_start[lo + 1]; ++k) {
      const std::uint32_t h0 = bucket[k];
      const auto [c, d] = ends(h0);
      if (std::max(c, d) != hi) continue;
      if (h0 == h) {
        edge_of[h] = static_cast<std::uint32_t>(edges.size());
        edges.push_back({lo, hi});
      } else {
        edge_of[h] = edge_of[h0];
      }
      break;
    }
  }
  r.edges = edges.size();
  // The half-edges on each edge, in order.
  std::vector<std::uint32_t> halves_start(edges.size() + 1, 0);
  for (std::size_t h = 0; h < nh; ++h) {
    if (edge_of[h] != kNoEdge) ++halves_start[edge_of[h] + 1];
  }
  for (std::size_t e = 0; e < edges.size(); ++e) halves_start[e + 1] += halves_start[e];
  std::vector<std::uint32_t> halves(halves_start[edges.size()]);
  {
    std::vector<std::uint32_t> fill(halves_start.begin(), halves_start.end() - 1);
    for (std::size_t h = 0; h < nh; ++h) {
      if (edge_of[h] != kNoEdge) halves[fill[edge_of[h]]++] = static_cast<std::uint32_t>(h);
    }
  }

  const auto from = [&](std::uint32_t h) { return mesh.indices[h]; };
  UnionFind shells(tri_count);
  for (std::size_t e = 0; e < edges.size(); ++e) {
    const std::uint32_t b = halves_start[e];
    const std::uint32_t count = halves_start[e + 1] - b;
    for (std::uint32_t i = 1; i < count; ++i) shells.unite(halves[b] / 3, halves[b + i] / 3);
    if (count == 1) {
      r.boundary_edges.push_back(edges[e]);
    } else if (count > 2) {
      r.non_manifold_edges.push_back(edges[e]);
    } else if (from(halves[b]) == from(halves[b + 1])) {
      r.inconsistent_edges.push_back(edges[e]);
    }
  }
  std::vector<bool> has_tri(tri_count, false);
  for (std::uint32_t t = 0; t < tri_count; ++t) has_tri[shells.find(t)] = true;
  r.shells = static_cast<std::size_t>(std::count(has_tri.begin(), has_tri.end(), true));

  // Fans: around each vertex, corners of triangles joined across edges at
  // that vertex that have exactly two triangles.
  std::vector<std::uint32_t> corner_start(nv + 1, 0);
  const auto degenerate = [&](const Triangle& tri) {
    return tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2];
  };
  for (std::uint32_t t = 0; t < tri_count; ++t) {
    const Triangle tri = mesh.triangle(t);
    if (degenerate(tri)) continue;
    for (const std::uint32_t v : tri) ++corner_start[v + 1];
  }
  for (std::size_t v = 0; v < nv; ++v) corner_start[v + 1] += corner_start[v];
  std::vector<std::uint32_t> corners(corner_start[nv]);
  {
    std::vector<std::uint32_t> fill(corner_start.begin(), corner_start.end() - 1);
    for (std::uint32_t t = 0; t < tri_count; ++t) {
      const Triangle tri = mesh.triangle(t);
      if (degenerate(tri)) continue;
      for (const std::uint32_t v : tri) corners[fill[v]++] = t;
    }
  }
  std::vector<std::uint32_t> parent;
  for (std::uint32_t v = 0; v < nv; ++v) {
    const std::uint32_t* tris = corners.data() + corner_start[v];
    const std::uint32_t n = corner_start[v + 1] - corner_start[v];
    if (n < 2) continue;
    // Local union-find over this vertex's triangles, joined through the
    // edges at v that two of them share.
    parent.resize(n);
    std::iota(parent.begin(), parent.end(), 0u);
    const auto find = [&](std::uint32_t x) {
      while (parent[x] != x) x = parent[x] = parent[parent[x]];
      return x;
    };
    const auto local = [&](std::uint32_t t) {
      std::uint32_t i = 0;
      while (tris[i] != t) ++i;
      return i;
    };
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint32_t t = tris[i];
      for (std::uint32_t k = 0; k < 3; ++k) {
        const std::uint32_t h = 3 * t + k;
        const auto [a, b] = ends(h);
        if (a != v && b != v) continue;
        const std::uint32_t e = edge_of[h];
        if (halves_start[e + 1] - halves_start[e] != 2) continue;
        const std::uint32_t x = find(local(halves[halves_start[e]] / 3));
        const std::uint32_t y = find(local(halves[halves_start[e] + 1] / 3));
        if (x != y) parent[std::max(x, y)] = std::min(x, y);
      }
    }
    std::uint32_t roots = 0;
    for (std::uint32_t i = 0; i < n; ++i) if (find(i) == i) ++roots;
    if (roots > 1) r.non_manifold_vertices.push_back(v);
  }
  return r;
}

}  // namespace vs::mesh

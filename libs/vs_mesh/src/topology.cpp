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
  TopologyReport r;
  const std::size_t tri_count = mesh.triangle_count();
  r.triangles = tri_count;

  // Edges, in a deterministic order (first use).
  std::unordered_map<std::uint64_t, std::uint32_t> edge_index;
  std::vector<Edge> edges;
  std::vector<std::vector<std::uint32_t>> edge_halves;  // half-edges on each edge
  edge_index.reserve(mesh.indices.size());
  std::vector<bool> referenced(mesh.vertex_count(), false);
  for (std::uint32_t t = 0; t < tri_count; ++t) {
    const Triangle tri = mesh.triangle(t);
    for (const std::uint32_t v : tri) referenced[v] = true;
    if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) {
      r.degenerate_triangles.push_back(t);
    } else if (double_area(mesh, t) == 0.0) {
      ++r.zero_area_triangles;
    }
    for (std::uint32_t k = 0; k < 3; ++k) {
      const std::uint32_t a = tri[k];
      const std::uint32_t b = tri[(k + 1) % 3];
      if (a == b) continue;
      const auto [it, added] =
          edge_index.try_emplace(edge_key(a, b), static_cast<std::uint32_t>(edges.size()));
      if (added) {
        edges.push_back({std::min(a, b), std::max(a, b)});
        edge_halves.emplace_back();
      }
      edge_halves[it->second].push_back(3 * t + k);
    }
  }
  r.vertices = static_cast<std::size_t>(std::count(referenced.begin(), referenced.end(), true));
  r.edges = edges.size();

  const auto from = [&](std::uint32_t h) { return mesh.indices[h]; };
  UnionFind shells(tri_count);
  for (std::size_t e = 0; e < edges.size(); ++e) {
    const auto& hs = edge_halves[e];
    for (std::size_t i = 1; i < hs.size(); ++i) shells.unite(hs[0] / 3, hs[i] / 3);
    if (hs.size() == 1) {
      r.boundary_edges.push_back(edges[e]);
    } else if (hs.size() > 2) {
      r.non_manifold_edges.push_back(edges[e]);
    } else if (from(hs[0]) == from(hs[1])) {
      r.inconsistent_edges.push_back(edges[e]);
    }
  }
  std::vector<bool> has_tri(tri_count, false);
  for (std::uint32_t t = 0; t < tri_count; ++t) has_tri[shells.find(t)] = true;
  r.shells = static_cast<std::size_t>(std::count(has_tri.begin(), has_tri.end(), true));

  // Fans: around each vertex, corners of triangles joined across edges at
  // that vertex that have exactly two triangles.
  std::vector<std::vector<std::uint32_t>> corners(mesh.vertex_count());
  for (std::uint32_t t = 0; t < tri_count; ++t) {
    const Triangle tri = mesh.triangle(t);
    if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) continue;
    for (std::uint32_t k = 0; k < 3; ++k) corners[tri[k]].push_back(t);
  }
  for (std::uint32_t v = 0; v < corners.size(); ++v) {
    const auto& tris = corners[v];
    if (tris.size() < 2) continue;
    // Local union-find over this vertex's triangles, joined through the
    // edges at v that two of them share.
    std::unordered_map<std::uint32_t, std::uint32_t> local;
    for (std::uint32_t i = 0; i < tris.size(); ++i) local.emplace(tris[i], i);
    UnionFind fans(tris.size());
    for (const std::uint32_t t : tris) {
      const Triangle tri = mesh.triangle(t);
      for (std::uint32_t k = 0; k < 3; ++k) {
        const std::uint32_t a = tri[k];
        const std::uint32_t b = tri[(k + 1) % 3];
        if (a != v && b != v) continue;
        const auto& hs = edge_halves[edge_index.at(edge_key(a, b))];
        if (hs.size() != 2) continue;
        fans.unite(local.at(hs[0] / 3), local.at(hs[1] / 3));
      }
    }
    std::uint32_t roots = 0;
    for (std::uint32_t i = 0; i < tris.size(); ++i) if (fans.find(i) == i) ++roots;
    if (roots > 1) r.non_manifold_vertices.push_back(v);
  }
  return r;
}

}  // namespace vs::mesh

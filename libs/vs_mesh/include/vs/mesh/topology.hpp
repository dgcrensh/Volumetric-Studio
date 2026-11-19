#pragma once

// Half-edge utilities and the watertight and manifold checks every mesh in
// the studio must pass before it is exported (spec, "Combined mesh").
//
// Definitions, all structural (by vertex index, never by distance):
//   - an edge is an unordered pair of vertices used by at least one triangle;
//   - a boundary edge is used by exactly one triangle;
//   - a non-manifold edge is used by three or more triangles;
//   - an inconsistent edge is used by two triangles that run along it the
//     same way, so their orientations disagree;
//   - a non-manifold vertex is one whose surrounding triangles fall into
//     more than one fan (two cones touching at their tips, say);
//   - a degenerate triangle repeats a vertex.
// A mesh is watertight when it has no boundary edges, and manifold when it
// has no non-manifold or inconsistent edges, no non-manifold vertices and
// no degenerate triangles. Zero-area triangles are counted but don't break
// either property: they are a quality warning, not a topological defect.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "vs/mesh/mesh.hpp"

namespace vs::mesh {

// Half-edges over a mesh's triangles: half-edge 3t+k runs from corner k of
// triangle t to corner k+1. `twin` pairs it with the half-edge running the
// other way along the same edge, when there is exactly one such and no
// third triangle uses the edge; kNone otherwise.
class HalfEdges {
 public:
  static constexpr std::uint32_t kNone = 0xffffffffu;

  explicit HalfEdges(const Mesh& mesh);

  [[nodiscard]] std::size_t size() const { return from_.size(); }
  [[nodiscard]] std::uint32_t from(std::uint32_t h) const { return from_[h]; }
  [[nodiscard]] std::uint32_t to(std::uint32_t h) const { return from_[next(h)]; }
  [[nodiscard]] static std::uint32_t next(std::uint32_t h) { return h % 3 == 2 ? h - 2 : h + 1; }
  [[nodiscard]] static std::uint32_t prev(std::uint32_t h) { return h % 3 == 0 ? h + 2 : h - 1; }
  [[nodiscard]] static std::uint32_t face(std::uint32_t h) { return h / 3; }
  [[nodiscard]] std::uint32_t twin(std::uint32_t h) const { return twin_[h]; }
  // How many half-edges (either direction) lie on h's edge.
  [[nodiscard]] std::uint32_t edge_use(std::uint32_t h) const { return use_[h]; }

 private:
  std::vector<std::uint32_t> from_;
  std::vector<std::uint32_t> twin_;
  std::vector<std::uint32_t> use_;
};

struct Edge {
  std::uint32_t a = 0;
  std::uint32_t b = 0;  // a < b
  friend bool operator==(const Edge&, const Edge&) = default;
};

struct TopologyReport {
  std::size_t vertices = 0;  // referenced by a triangle
  std::size_t edges = 0;
  std::size_t triangles = 0;
  std::size_t shells = 0;    // edge-connected components of triangles
  std::vector<Edge> boundary_edges;
  std::vector<Edge> non_manifold_edges;
  std::vector<Edge> inconsistent_edges;
  std::vector<std::uint32_t> non_manifold_vertices;
  std::vector<std::uint32_t> degenerate_triangles;
  std::size_t zero_area_triangles = 0;

  [[nodiscard]] bool watertight() const { return boundary_edges.empty(); }
  [[nodiscard]] bool manifold() const {
    return non_manifold_edges.empty() && inconsistent_edges.empty() &&
           non_manifold_vertices.empty() && degenerate_triangles.empty();
  }
  // V − E + F over referenced vertices; 2 per closed shell of genus 0.
  [[nodiscard]] long long euler_characteristic() const {
    return static_cast<long long>(vertices) - static_cast<long long>(edges) +
           static_cast<long long>(triangles);
  }
};

// Examines the mesh's triangles. The mesh must pass Mesh::check().
TopologyReport analyse(const Mesh& mesh);

}  // namespace vs::mesh

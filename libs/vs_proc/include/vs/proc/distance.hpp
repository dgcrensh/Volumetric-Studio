#pragma once

// Distance to a triangle mesh, for fields made from B-rep inputs (plan,
// M10: "boundary as SDF, computed from the B-rep"; queries "distance to a
// connection surface").
//
// The B-rep is tessellated once, on the calling thread (inputs.hpp); after
// that every query is plain arithmetic on this structure, safe from any
// number of threads and independent of the kernel. A bounding volume
// hierarchy (median split on triangle centroids, built deterministically)
// finds the nearest triangle; ties keep the first one found in a fixed
// traversal order, so a query's answer never depends on the thread.
//
// Signed distance. For a closed, consistently wound mesh (counter-clockwise
// from outside, as vs_mesh::tessellate() makes them) the sign comes from
// the angle-weighted pseudonormal of the nearest feature (face, edge or
// vertex; Bærentzen and Aanæs 2005): negative inside, positive outside.
// That is exact for every point, not only near the surface. An open mesh
// (a connection surface's patch) gives unsigned distances only.
//
// Accuracy. Distances are to the tessellation, computed in FP64 from its
// FP32 positions. For planar faces that is the exact surface; for curved
// faces it is within the tessellation's deflection.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "vs/core/box.hpp"
#include "vs/core/hash.hpp"
#include "vs/core/vec.hpp"
#include "vs/mesh/mesh.hpp"

namespace vs::proc {

using core::dbox3;
using core::dvec3;

class TriangleDistance {
 public:
  TriangleDistance() = default;
  // `closed`: the mesh is closed and consistently wound, so signed()
  // works. The caller says so (vs_mesh::analyse() can tell).
  TriangleDistance(const mesh::Mesh& mesh, bool closed);

  [[nodiscard]] bool empty() const { return triangles_.empty(); }
  [[nodiscard]] bool closed() const { return closed_; }
  [[nodiscard]] std::size_t triangle_count() const { return triangles_.size(); }
  [[nodiscard]] const dbox3& bounds() const { return bounds_; }
  // A digest of the positions and triangles: what identifies the surface.
  [[nodiscard]] const core::Hash256& hash() const { return hash_; }

  // The mesh it was built from (positions in FP64, triangles in input
  // order): scatter.surface samples a patch's triangles by area.
  [[nodiscard]] const std::vector<dvec3>& positions() const { return positions_; }
  [[nodiscard]] std::array<std::uint32_t, 3> triangle(std::size_t t) const {
    return {triangles_[t].v[0], triangles_[t].v[1], triangles_[t].v[2]};
  }

  // Unsigned distance to the nearest triangle; +infinity when empty.
  [[nodiscard]] double distance(const dvec3& p) const;
  // Signed distance (negative inside). For an open mesh, the unsigned one.
  [[nodiscard]] double signed_distance(const dvec3& p) const;

  struct Nearest {
    double distance_squared = 0.0;
    dvec3 point{};
    std::size_t triangle = 0;
    // Which feature of the triangle the point is on: 0 the face, 1..3 the
    // edges (v0v1, v1v2, v2v0), 4..6 the vertices (v0, v1, v2).
    int feature = 0;
  };
  [[nodiscard]] Nearest nearest(const dvec3& p) const;

 private:
  struct Node {
    dbox3 box;
    std::uint32_t first = 0;  // leaves: triangles [first, first + count)
    std::uint32_t count = 0;  // 0: an inner node; children at left, left + 1
    std::uint32_t left = 0;
  };
  struct Tri {
    std::uint32_t v[3];
    dvec3 normal;           // unit face normal
    dvec3 edge_normal[3];   // pseudonormals of edges v0v1, v1v2, v2v0
  };

  void build(std::uint32_t node, std::uint32_t first, std::uint32_t count, int depth);
  void search(std::uint32_t node, const dvec3& p, Nearest& best) const;

  std::vector<dvec3> positions_;
  std::vector<dvec3> vertex_normals_;  // angle-weighted
  std::vector<Tri> triangles_;
  std::vector<std::uint32_t> order_;   // triangles in leaf order
  std::vector<Node> nodes_;
  dbox3 bounds_;
  core::Hash256 hash_{};
  bool closed_ = false;
};

}  // namespace vs::proc

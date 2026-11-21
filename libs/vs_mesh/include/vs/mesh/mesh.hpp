#pragma once

// Triangle meshes: what every mesher in the studio produces (spec,
// "Geometry and precision": generated geometry is FP32).
//
// A Mesh is indexed triangles over FP32 positions. Every triangle carries
// the id of the volume it belongs to, so a combined mesh still knows which
// volume each part came from (3MF export per volume, diagnostics). Every
// vertex may carry the id of the seam vertex it stands for (seam.hpp):
// welding merges vertices by that id, never by distance (weld.hpp).
//
// Invariants (check() reports breaks as mesh.invalid):
//   - indices.size() is a multiple of three, and every index is in range;
//   - seam_vertices.size() == positions.size() (Id::null(): not on a seam);
//   - triangle_volumes.size() == triangle_count();
//   - every position is finite (H4).
// Triangles wind counter-clockwise seen from outside the volume.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "vs/core/box.hpp"
#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/core/vec.hpp"
#include "vs/validate/containment.hpp"

namespace vs::mesh {

using core::Diagnostic;
using core::Diagnostics;
using core::fvec3;
using core::Id;

using Triangle = std::array<std::uint32_t, 3>;

struct Mesh {
  std::vector<fvec3> positions;
  std::vector<Id> seam_vertices;      // per vertex
  std::vector<std::uint32_t> indices;  // three per triangle
  std::vector<Id> triangle_volumes;   // per triangle

  [[nodiscard]] std::size_t vertex_count() const { return positions.size(); }
  [[nodiscard]] std::size_t triangle_count() const { return indices.size() / 3; }
  [[nodiscard]] Triangle triangle(std::size_t t) const {
    return {indices[3 * t], indices[3 * t + 1], indices[3 * t + 2]};
  }
  [[nodiscard]] bool empty() const { return indices.empty(); }

  // Adds a vertex and returns its index.
  std::uint32_t add_vertex(const fvec3& position, const Id& seam_vertex = Id::null());
  void add_triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c, const Id& volume);
  void add_triangle(const Triangle& t, const Id& volume) { add_triangle(t[0], t[1], t[2], volume); }

  // Appends `other`, offsetting its indices. Nothing is merged. `other`
  // may be this mesh.
  void append(const Mesh& other);

  [[nodiscard]] core::fbox3 bounds() const;

  // The bytes its arrays hold (capacity, not size): what caches count (H5).
  [[nodiscard]] std::size_t memory_bytes() const {
    return positions.capacity() * sizeof(fvec3) + seam_vertices.capacity() * sizeof(Id) +
           indices.capacity() * sizeof(std::uint32_t) + triangle_volumes.capacity() * sizeof(Id);
  }

  // The volumes its triangles belong to, each once, in order of first use.
  [[nodiscard]] std::vector<Id> volumes() const;

  // Structural invariants above; `subject` names the mesh in diagnostics.
  [[nodiscard]] Diagnostics check(const core::SubjectRef& subject = {}) const;

  friend bool operator==(const Mesh&, const Mesh&) = default;
};

// The mesh as vs_validate's containment check takes it.
validate::MeshView view(const Mesh& mesh);

// Twice the area of a triangle, from FP32 positions, computed in FP64.
double double_area(const Mesh& mesh, std::size_t triangle);

// Signed volume enclosed by a closed, consistently wound mesh, mm³
// (positive when triangles face out). Summed over tetrahedra to one of its
// vertices, so it keeps its digits far from the origin.
double enclosed_volume(const Mesh& mesh);

}  // namespace vs::mesh

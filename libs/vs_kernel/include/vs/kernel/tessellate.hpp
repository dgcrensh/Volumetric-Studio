#pragma once

// B-rep tessellation as plain data (M8), for vs_mesh's conforming
// tessellator. The kernel knows nothing about seams: it meshes a shape
// with OCCT's BRepMesh and hands back, structurally, which point of each
// face's triangulation lies on which edge or vertex, so a caller can keep
// most faces as they are and re-triangulate the few whose edges it wants
// divided differently (vs_mesh/tessellate.hpp).
//
// tessellate() first merges faces and edges lying on the same surface or
// curve (so a face split by a boolean along a line comes back whole),
// splits faces along the outlines of any `imprints` lying on them (L2),
// copies the shape (meshing attaches triangulations to the shape, and
// handles are immutable and may be shared between threads), then runs
// BRepMesh with `params`. The result describes:
//
//   - vertices: every B-rep vertex, once;
//   - edges: every edge, once, with BRepMesh's division of it: ascending
//     curve parameters from its first vertex to its last, and their points
//     (the ends are exactly the vertices' points; a degenerate edge, such
//     as a sphere's pole, has every point at its vertex);
//   - faces: every face, once, with its wires as ordered edge uses (outer
//     wire first; the seam of a closed surface appears twice, once each
//     way; degenerate edges are included), and BRepMesh's triangulation:
//     points, (u, v) parameters, triangles wound counter-clockwise seen
//     from outside the solid, and for each point where it sits (inside the
//     face, on a vertex, or the k-th point of an edge's division).
//
// Codes:
//   kernel.tessellate.empty    the shape has no faces
//   kernel.tessellate.params   a deflection or angle that isn't positive and finite
//   kernel.tessellate.failed   BRepMesh failed, or left a face or edge unmeshed

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "vs/core/settings.hpp"
#include "vs/core/vec.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/kernel/shape.hpp"

namespace vs::kernel {

using core::dvec2;
using core::dvec3;

struct TessellationParams {
  // The largest distance between a triangle and the surface, mm.
  double deflection = 0.02;
  // The largest angle between neighbouring segments of an edge, or the
  // normals of neighbouring triangles, radians.
  double angle = 0.3;
};

// Where a point of a face's triangulation sits.
struct TessNode {
  enum class Kind : std::uint8_t { Interior, Vertex, Edge };
  Kind kind = Kind::Interior;
  std::uint32_t index = 0;  // Vertex: the vertex; Edge: the edge
  std::uint32_t k = 0;      // Edge: the point of the edge's division
};

struct TessEdge {
  Edge edge;
  std::uint32_t first_vertex = 0;
  std::uint32_t last_vertex = 0;  // == first_vertex for a closed edge (a full circle)
  bool degenerate = false;
  std::vector<double> params;  // ascending
  std::vector<dvec3> points;   // one per parameter
};

// An edge as a wire runs along it. `reversed`: the wire runs from the
// edge's last vertex to its first (down its parameters). `oriented` is the
// edge as the face holds it, for edge_uv().
struct TessEdgeUse {
  std::uint32_t edge = 0;
  bool reversed = false;
  Edge oriented;
};

struct TessFace {
  Face face;
  // Planar faces: the plane, normal out of the material.
  std::optional<Plane> plane;
  // Cylindrical faces (L2): the cylinder, `inward` when the normal out of
  // the material points towards the axis.
  std::optional<Cylinder> cylinder;
  std::vector<std::vector<TessEdgeUse>> wires;  // outer first
  // BRepMesh's triangulation of the face.
  std::vector<dvec3> points;
  std::vector<dvec2> uv;
  std::vector<TessNode> nodes;
  std::vector<std::array<std::uint32_t, 3>> triangles;  // counter-clockwise from outside
  // True when a triangle that runs counter-clockwise in (u, v) faces out of
  // the material (the face isn't reversed on its surface).
  bool uv_outward = true;
  // The surface's speed in u and v (mm per unit) at the middle of the
  // face's parameter range: scaling (u, v) by it makes the parameter plane
  // roughly isometric to the face there.
  dvec2 uv_scale{1.0, 1.0};
};

struct Tessellation {
  std::vector<dvec3> vertices;
  std::vector<TessEdge> edges;
  std::vector<TessFace> faces;
};

// `imprints` (L2): faces lying on faces of `shape`, whose outlines split
// those faces before meshing, so a curved connection surface's patch is a
// face (or several) of its own. A face that doesn't lie on the shape
// changes nothing.
core::Result<Tessellation> tessellate(const Shape& shape, const TessellationParams& params,
                                      const core::ProductSettings& settings,
                                      std::span<const Face> imprints = {});

// The parameter of the point of `edge` nearest `point`, when that point is
// within `tolerance` of it and within the edge's ends; nullopt otherwise.
// For a closed edge, a point at its vertex gives the first parameter.
std::optional<double> edge_parameter(const Edge& edge, const dvec3& point, double tolerance);

// The point of `edge` at parameter `t`.
dvec3 edge_point(const Edge& edge, double t);

// The (u, v) of the point at parameter `t` of an edge use (`oriented`, from
// TessEdgeUse) on `face`. A closed surface's seam has a different (u, v)
// for each of its two uses.
dvec2 edge_uv(const Face& face, const Edge& oriented, double t);

// The point of `face`'s surface at parameters `uv` (L2: points a caller
// adds inside a face it re-triangulates).
dvec3 face_point(const Face& face, const dvec2& uv);

}  // namespace vs::kernel

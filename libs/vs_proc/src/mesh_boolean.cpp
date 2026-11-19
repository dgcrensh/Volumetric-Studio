#include "mesh_boolean.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

#include <manifold/manifold.h>

namespace vs::proc::detail {

namespace {

manifold::MeshGL to_gl(const mesh::Mesh& m) {
  manifold::MeshGL gl;
  gl.numProp = 3;
  gl.vertProperties.reserve(3 * m.positions.size());
  for (const auto& p : m.positions) {
    gl.vertProperties.push_back(p.x);
    gl.vertProperties.push_back(p.y);
    gl.vertProperties.push_back(p.z);
  }
  gl.triVerts.assign(m.indices.begin(), m.indices.end());
  gl.Merge();  // joins vertices that share a position (seams, STL corners)
  return gl;
}

std::string why(manifold::Manifold::Error e) {
  switch (e) {
    case manifold::Manifold::Error::NoError: return "no error";
    case manifold::Manifold::Error::NonFiniteVertex: return "it has a vertex that isn't finite";
    case manifold::Manifold::Error::NotManifold:
      return "it isn't a closed, consistently oriented manifold";
    case manifold::Manifold::Error::VertexOutOfBounds: return "a triangle names a missing vertex";
    default: return "Manifold can't use it";
  }
}

// Manifold orders its output by run ids from a process-wide counter, so
// the same boolean can come back in another order. Canonical order: each
// triangle rotated to start at its lowest corner (orientation kept),
// triangles sorted by their corners, vertices numbered in order of use.
mesh::Mesh canonical(const manifold::MeshGL& gl, const core::Id& volume) {
  const std::size_t stride = gl.numProp;
  const auto at = [&](std::uint32_t v) {
    return std::array<float, 3>{gl.vertProperties[v * stride], gl.vertProperties[v * stride + 1],
                                gl.vertProperties[v * stride + 2]};
  };
  using Tri = std::array<std::uint32_t, 3>;
  std::vector<Tri> tris;
  tris.reserve(gl.triVerts.size() / 3);
  for (std::size_t t = 0; t + 2 < gl.triVerts.size(); t += 3) {
    Tri tri{gl.triVerts[t], gl.triVerts[t + 1], gl.triVerts[t + 2]};
    std::size_t lowest = 0;
    for (std::size_t k = 1; k < 3; ++k) {
      if (at(tri[k]) < at(tri[lowest])) lowest = k;
    }
    std::rotate(tri.begin(), tri.begin() + static_cast<std::ptrdiff_t>(lowest), tri.end());
    tris.push_back(tri);
  }
  std::sort(tris.begin(), tris.end(), [&](const Tri& x, const Tri& y) {
    for (std::size_t k = 0; k < 3; ++k) {
      const auto px = at(x[k]);
      const auto py = at(y[k]);
      if (px != py) return px < py;
    }
    return false;
  });
  mesh::Mesh m;
  std::vector<std::uint32_t> renumber(stride == 0 ? 0 : gl.vertProperties.size() / stride,
                                      0xffffffffu);
  for (const Tri& tri : tris) {
    std::array<std::uint32_t, 3> v{};
    for (std::size_t k = 0; k < 3; ++k) {
      std::uint32_t& r = renumber[tri[k]];
      if (r == 0xffffffffu) {
        const auto p = at(tri[k]);
        r = m.add_vertex({p[0], p[1], p[2]});
      }
      v[k] = r;
    }
    m.add_triangle(v[0], v[1], v[2], volume);
  }
  return m;
}

}  // namespace

BooleanResult mesh_boolean(const mesh::Mesh& a, const mesh::Mesh& b, BooleanOp op,
                           const core::Id& volume) {
  BooleanResult out;
  const manifold::Manifold ma(to_gl(a));
  if (ma.Status() != manifold::Manifold::Error::NoError) {
    out.bad_input = 1;
    out.message = why(ma.Status());
    return out;
  }
  const manifold::Manifold mb(to_gl(b));
  if (mb.Status() != manifold::Manifold::Error::NoError) {
    out.bad_input = 2;
    out.message = why(mb.Status());
    return out;
  }
  const manifold::OpType type = op == BooleanOp::Union          ? manifold::OpType::Add
                                : op == BooleanOp::Difference ? manifold::OpType::Subtract
                                                              : manifold::OpType::Intersect;
  const manifold::Manifold r = ma.Boolean(mb, type);
  if (r.Status() != manifold::Manifold::Error::NoError) {
    out.message = why(r.Status());
    return out;
  }
  out.mesh = canonical(r.GetMeshGL(), volume);
  return out;
}

}  // namespace vs::proc::detail

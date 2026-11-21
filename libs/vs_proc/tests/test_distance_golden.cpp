// Golden hashes of TriangleDistance (plan H3): its BVH, pseudonormals and
// boundary-distance samples, identical on every platform and standard
// library. CI runs this on Linux (GCC and Clang, libstdc++) and Windows
// (MSVC and its STL), so all three must agree.
//
// The meshes are built from literals and sin_det() / cos_det() (never
// libm), positions in FP32 as the engine's tessellations are, and the
// sample points come from an integer generator, not <random>'s
// distributions (whose output the standard leaves to the library). One
// mesh sits near the corner of the ±1,000 mm working space, where FP32
// positions are coarsest.
//
// If one of these fails, a distance or the tie-breaking between equally
// near triangles changed. That changes procedural contents for identical
// inputs, so it needs a deliberate decision and a bump of
// core::kEngineVersion, never a quick update of the expected value.

#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "vs/core/hash.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/proc/distance.hpp"
#include "vs/proc/field.hpp"

using namespace vs;
using core::dvec3;
using core::fvec3;

namespace {

const core::Id kVolume = core::Id::derive(core::Id::null(), "golden");

// A UV sphere: `stacks` bands between the poles, `slices` round them.
mesh::Mesh uv_sphere(const dvec3& centre, double radius, int stacks, int slices) {
  constexpr double kPi = 3.14159265358979311600;
  mesh::Mesh m;
  const auto at = [&](double polar, double azimuth) {
    const double s = proc::sin_det(polar);
    return core::to_fp32(centre + dvec3{radius * s * proc::cos_det(azimuth),
                                        radius * s * proc::sin_det(azimuth),
                                        radius * proc::cos_det(polar)});
  };
  const std::uint32_t north = m.add_vertex(core::to_fp32(centre + dvec3{0, 0, radius}));
  for (int i = 1; i < stacks; ++i) {
    for (int j = 0; j < slices; ++j) {
      m.add_vertex(at(kPi * i / stacks, 2.0 * kPi * j / slices));
    }
  }
  const std::uint32_t south = m.add_vertex(core::to_fp32(centre - dvec3{0, 0, radius}));
  const auto ring = [&](int i, int j) {
    return static_cast<std::uint32_t>(1 + (i - 1) * slices + (j % slices));
  };
  for (int j = 0; j < slices; ++j) {
    m.add_triangle(north, ring(1, j), ring(1, j + 1), kVolume);
    for (int i = 1; i + 1 < stacks; ++i) {
      m.add_triangle(ring(i, j), ring(i + 1, j), ring(i + 1, j + 1), kVolume);
      m.add_triangle(ring(i, j), ring(i + 1, j + 1), ring(i, j + 1), kVolume);
    }
    m.add_triangle(south, ring(stacks - 1, j + 1), ring(stacks - 1, j), kVolume);
  }
  return m;
}

// An axis-aligned box, each side split into a 2 x 2 grid of squares, each
// square into two triangles: many triangles equally near a point, so the
// answer rests on the BVH's leaf order.
mesh::Mesh split_box(const dvec3& lo, const dvec3& hi) {
  mesh::Mesh m;
  const dvec3 size = hi - lo;
  // Side (axis, sign): u and v along the other two axes, wound so the
  // normal points out.
  for (int axis = 0; axis < 3; ++axis) {
    for (int side = 0; side < 2; ++side) {
      const int ua = (axis + 1) % 3;
      const int va = (axis + 2) % 3;
      const auto point = [&](int u, int v) {
        double c[3] = {lo.x, lo.y, lo.z};
        const double s[3] = {size.x, size.y, size.z};
        c[axis] += side * s[axis];
        c[ua] += 0.5 * u * s[ua];
        c[va] += 0.5 * v * s[va];
        return m.add_vertex(core::to_fp32(dvec3{c[0], c[1], c[2]}));
      };
      for (int u = 0; u < 2; ++u) {
        for (int v = 0; v < 2; ++v) {
          const std::uint32_t a = point(u, v), b = point(u + 1, v), c = point(u + 1, v + 1),
                              d = point(u, v + 1);
          if (side == 1) {
            m.add_triangle(a, b, c, kVolume);
            m.add_triangle(a, c, d, kVolume);
          } else {
            m.add_triangle(a, c, b, kVolume);
            m.add_triangle(a, d, c, kVolume);
          }
        }
      }
    }
  }
  return m;
}

// An open, gently folded sheet (a connection surface's patch).
mesh::Mesh folded_sheet() {
  mesh::Mesh m;
  constexpr int n = 9;
  for (int j = 0; j <= n; ++j) {
    for (int i = 0; i <= n; ++i) {
      const double x = -20.0 + 5.0 * i;
      const double y = -20.0 + 5.0 * j;
      m.add_vertex(core::to_fp32(dvec3{x, y, 0.125 * (i % 3) + 0.25 * (j % 2)}));
    }
  }
  for (int j = 0; j < n; ++j) {
    for (int i = 0; i < n; ++i) {
      const auto v = [&](int a, int b) { return static_cast<std::uint32_t>(b * (n + 1) + a); };
      m.add_triangle(v(i, j), v(i + 1, j), v(i + 1, j + 1), kVolume);
      m.add_triangle(v(i, j), v(i + 1, j + 1), v(i, j + 1), kVolume);
    }
  }
  return m;
}

// The BVH and the pseudonormals.
core::Hash256 structure_hash(const proc::TriangleDistance& d) {
  core::Hasher h;
  h.add(static_cast<std::uint64_t>(d.nodes().size()));
  for (const auto& n : d.nodes()) h.add(n.box.min).add(n.box.max).add(n.first).add(n.count).add(n.left);
  h.add(static_cast<std::uint64_t>(d.order().size()));
  for (const auto t : d.order()) h.add(t);
  return h.finalize();
}

core::Hash256 normals_hash(const proc::TriangleDistance& d) {
  core::Hasher h;
  for (const auto& n : d.vertex_normals()) h.add(n);
  for (const auto& t : d.triangles()) {
    h.add(t.normal);
    for (const auto& e : t.edge_normal) h.add(e);
  }
  return h.finalize();
}

// Distances at a grid over the mesh's box (grown by a quarter of its
// size) and at pseudo-random points: signed and unsigned distance, and the
// nearest triangle, feature and point.
core::Hash256 samples_hash(const proc::TriangleDistance& d) {
  const core::dbox3 b = d.bounds();
  const dvec3 lo = b.min - b.size() * 0.25;
  const dvec3 step = b.size() * (1.5 / 16.0);
  core::Hasher h;
  const auto sample = [&](const dvec3& p) {
    const auto n = d.nearest(p);
    h.add(p).add(d.signed_distance(p)).add(d.distance(p));
    h.add(static_cast<std::uint64_t>(n.triangle)).add(n.feature).add(n.point);
  };
  for (int k = 0; k <= 16; ++k) {
    for (int j = 0; j <= 16; ++j) {
      for (int i = 0; i <= 16; ++i) sample(lo + dvec3{step.x * i, step.y * j, step.z * k});
    }
  }
  std::uint64_t state = 0x9e3779b97f4a7c15ull;
  const auto unit = [&]() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<double>(state >> 11) * 0x1.0p-53;
  };
  for (int i = 0; i < 2000; ++i) {
    const dvec3 u{unit(), unit(), unit()};
    sample(lo + dvec3{u.x * step.x * 16, u.y * step.y * 16, u.z * step.z * 16});
  }
  return h.finalize();
}

struct Golden {
  std::string name;
  core::Hash256 actual;
  std::string expected;
};

}  // namespace

TEST_CASE("Boundary-distance golden hashes are identical on every platform",
          "[proc][distance][golden]") {
  const proc::TriangleDistance sphere(uv_sphere({1.5, -2.25, 3.0}, 7.0, 12, 24), true);
  const proc::TriangleDistance corner(split_box({985, -999, 985}, {999, -985, 999.5}), true);
  const proc::TriangleDistance sheet(folded_sheet(), false);
  REQUIRE(sphere.triangle_count() == 2 * 24 * 11);
  REQUIRE(corner.triangle_count() == 48);
  REQUIRE(sheet.triangle_count() == 162);

  const Golden cases[] = {
      {"sphere structure", structure_hash(sphere),
       "339953b8f22759ce680b998c331e31ae06720ad01118ae7041f289dfbad26abd"},
      {"sphere normals", normals_hash(sphere),
       "6478e533d2ae23fc0bdf573a41e8b9cea7978f70fb38920d17f3cb0b2acc7ab0"},
      {"sphere samples", samples_hash(sphere),
       "d00d425517a520596693360c6e47c39f382dbc836b9e65495794829478232454"},
      {"corner structure", structure_hash(corner),
       "c9774ccc7305e5e7107423953719c1270e3d9e2351efd978d9582847e1432797"},
      {"corner normals", normals_hash(corner),
       "8ed4c410f1093a2421c443cbe980d56f5c806419be7561f4ff16759e15f3dd7c"},
      {"corner samples", samples_hash(corner),
       "c9eaa352b98154b41edfc66b6a035b2dd67a0f0af04102a76ab597cbd6e6aec4"},
      {"sheet structure", structure_hash(sheet),
       "1a288e04c8926ba5a1452c8fcac90cf0443d356999be618bff2db54ba9be5489"},
      {"sheet samples", samples_hash(sheet),
       "25265f63db3373227ab106194555d436e5a0f25d0f6baa1b9ca219b5971478dd"},
  };
  for (const auto& c : cases) {
    INFO(c.name << ": " << c.actual.to_hex());
    CHECK(c.actual.to_hex() == c.expected);
  }
}

#include <catch2/catch_test_macros.hpp>

#include "vs/mesh/mesh.hpp"
#include "vs/mesh/topology.hpp"

using vs::core::fvec3;
using vs::core::Id;
using vs::mesh::analyse;
using vs::mesh::HalfEdges;
using vs::mesh::Mesh;

namespace {

const Id kVol = Id::derive(Id::null(), "topology-test");

// A unit cube at `o`, outward-facing, 12 triangles.
Mesh cube(fvec3 o = {0, 0, 0}) {
  Mesh m;
  for (int k = 0; k < 8; ++k) {
    m.add_vertex({o.x + static_cast<float>(k & 1), o.y + static_cast<float>((k >> 1) & 1),
                  o.z + static_cast<float>((k >> 2) & 1)});
  }
  const std::uint32_t f[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                 {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (const auto& q : f) {
    m.add_triangle(q[0], q[1], q[2], kVol);
    m.add_triangle(q[0], q[2], q[3], kVol);
  }
  return m;
}

}  // namespace

TEST_CASE("A closed cube is watertight and manifold", "[mesh][topology]") {
  const Mesh m = cube();
  CHECK(m.check().empty());
  const auto r = analyse(m);
  CHECK(r.watertight());
  CHECK(r.manifold());
  CHECK(r.shells == 1);
  CHECK(r.vertices == 8);
  CHECK(r.edges == 18);
  CHECK(r.euler_characteristic() == 2);
  CHECK(vs::mesh::enclosed_volume(m) == 1.0);

  const HalfEdges he(m);
  for (std::uint32_t h = 0; h < he.size(); ++h) {
    REQUIRE(he.twin(h) != HalfEdges::kNone);
    CHECK(he.twin(he.twin(h)) == h);
    CHECK(he.from(he.twin(h)) == he.to(h));
    CHECK(he.edge_use(h) == 2);
    CHECK(HalfEdges::next(HalfEdges::next(HalfEdges::next(h))) == h);
    CHECK(HalfEdges::prev(HalfEdges::next(h)) == h);
  }
}

TEST_CASE("A missing face leaves boundary edges", "[mesh][topology]") {
  Mesh m = cube();
  m.indices.resize(m.indices.size() - 6);
  m.triangle_volumes.resize(m.triangle_volumes.size() - 2);
  const auto r = analyse(m);
  CHECK_FALSE(r.watertight());
  CHECK(r.manifold());
  CHECK(r.boundary_edges.size() == 4);
}

TEST_CASE("A flipped triangle makes inconsistent edges", "[mesh][topology]") {
  Mesh m = cube();
  std::swap(m.indices[1], m.indices[2]);
  const auto r = analyse(m);
  CHECK(r.watertight());
  CHECK_FALSE(r.manifold());
  CHECK(r.inconsistent_edges.size() == 3);
}

TEST_CASE("A fin on an edge makes it non-manifold", "[mesh][topology]") {
  Mesh m = cube();
  const auto extra = m.add_vertex({0.5f, -1.0f, 0.5f});
  m.add_triangle(0, 1, extra, kVol);
  const auto r = analyse(m);
  CHECK_FALSE(r.manifold());
  CHECK(r.non_manifold_edges.size() == 1);
  CHECK(r.boundary_edges.size() == 2);
}

TEST_CASE("Two cubes touching at a corner make a non-manifold vertex", "[mesh][topology]") {
  Mesh a = cube();
  Mesh b = cube({1, 1, 1});
  // Share the corner (1,1,1): b's vertex 0 becomes a's vertex 7.
  for (auto& i : b.indices) i += 8;
  a.positions.insert(a.positions.end(), b.positions.begin(), b.positions.end());
  a.seam_vertices.insert(a.seam_vertices.end(), b.seam_vertices.begin(), b.seam_vertices.end());
  for (auto& i : b.indices) {
    if (i == 8) i = 7;
  }
  a.indices.insert(a.indices.end(), b.indices.begin(), b.indices.end());
  a.triangle_volumes.insert(a.triangle_volumes.end(), b.triangle_volumes.begin(),
                            b.triangle_volumes.end());
  const auto r = analyse(a);
  CHECK(r.watertight());
  CHECK_FALSE(r.manifold());
  CHECK(r.non_manifold_vertices == std::vector<std::uint32_t>{7});
  CHECK(r.shells == 2);
}

TEST_CASE("Degenerate and zero-area triangles are counted", "[mesh][topology]") {
  Mesh m = cube();
  m.add_triangle(0, 0, 1, kVol);
  auto r = analyse(m);
  CHECK(r.degenerate_triangles == std::vector<std::uint32_t>{12});
  CHECK_FALSE(r.manifold());

  Mesh flat;
  flat.add_vertex({0, 0, 0});
  flat.add_vertex({1, 0, 0});
  flat.add_vertex({2, 0, 0});
  flat.add_triangle(0, 1, 2, kVol);
  r = analyse(flat);
  CHECK(r.zero_area_triangles == 1);
  CHECK(r.degenerate_triangles.empty());
}

TEST_CASE("Mesh::check reports broken invariants", "[mesh][topology]") {
  Mesh m = cube();
  m.indices.push_back(99);
  CHECK(m.check().has_errors());
  m = cube();
  m.indices[0] = 99;
  CHECK(m.check().with_code("mesh.invalid").size() == 1);
  m = cube();
  m.seam_vertices.pop_back();
  CHECK(m.check().has_errors());
  m = cube();
  m.triangle_volumes.pop_back();
  CHECK(m.check().has_errors());
}

TEST_CASE("append offsets indices, and view exposes the arrays", "[mesh][topology]") {
  Mesh a = cube();
  a.append(cube({3, 0, 0}));
  CHECK(a.vertex_count() == 16);
  CHECK(a.triangle_count() == 24);
  const auto r = analyse(a);
  CHECK(r.watertight());
  CHECK(r.manifold());
  CHECK(r.shells == 2);
  CHECK(a.volumes() == std::vector<Id>{kVol});
  const auto v = vs::mesh::view(a);
  CHECK(v.positions.size() == 16);
  CHECK(v.indices.size() == 72);
  CHECK(a.bounds().max.x == 4.0f);
}

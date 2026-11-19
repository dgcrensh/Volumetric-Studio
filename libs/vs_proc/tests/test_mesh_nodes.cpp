// M12 mesh nodes: primitives, instancing, measuring and Manifold booleans.

#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../src/mesh_boolean.hpp"
#include "scenes.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/proc/evaluate.hpp"
#include "vs/proc/inspect.hpp"

using namespace vs;
using namespace vs::proc;
using core::dvec3;
using Catch::Approx;

namespace {

// One volume for every inspection, so meshes carry the same volume id.
const GraphInputs& inputs() {
  static const GraphInputs in = scenes::bare_inputs();
  return in;
}

NodeInspection inspect(const Graph& g, const Id& node) { return inspect_node(g, inputs(), node); }

double measured(const Graph& g, const Id& measure, const char* port) {
  const NodeInspection m = inspect(g, measure);
  INFO(scenes::codes(m.diagnostics));
  REQUIRE(m.ok());
  return std::get<double>(*m.find(port));
}

}  // namespace

TEST_CASE("Mesh primitives are closed and measure right", "[proc][mesh_nodes]") {
  Graph g;
  const Id box = g.add("mesh.box", "Box", {{"size", Setting(dvec3{10, 20, 30})}});
  const Id cyl = g.add("mesh.cylinder", "Cylinder",
                       {{"radius", Setting(5.0)}, {"height", Setting(10.0)}, {"segments", Setting(64.0)},
                        {"axis", Setting(dvec3{1, 1, 0})}});
  const Id mb = g.add("mesh.measure", "Box size");
  g.link(box, "mesh", mb, "mesh");
  const Id mc = g.add("mesh.measure", "Cylinder size");
  g.link(cyl, "mesh", mc, "mesh");
  CHECK(measured(g, mb, "volume") == Approx(6000.0));
  CHECK(measured(g, mb, "area") == Approx(2 * (200.0 + 300.0 + 600.0)));
  CHECK(measured(g, mb, "triangles") == 12.0);
  // A 64-gon prism: slightly less than the cylinder.
  const double polygon = 0.5 * 64 * 25.0 * std::sin(2 * std::numbers::pi / 64);
  CHECK(measured(g, mc, "volume") == Approx(polygon * 10.0).epsilon(1e-6));
  for (const Id& n : {box, cyl}) {
    const NodeInspection ins = inspect(g, n);
    REQUIRE(ins.ok());
    const auto topology = mesh::analyse(*std::get<MeshValue>(*ins.find("mesh")));
    CHECK(topology.watertight());
    CHECK(topology.manifold());
  }
}

TEST_CASE("Mesh booleans go through Manifold", "[proc][mesh_nodes]") {
  Graph g;
  const Id a = g.add("mesh.box", "A", {{"size", Setting(dvec3{10, 10, 10})}});
  const Id b = g.add("mesh.box", "B", {{"centre", Setting(dvec3{5, 0, 0})}, {"size", Setting(dvec3{10, 10, 10})}});
  const Id u = g.add("mesh.union", "Union");
  const Id d = g.add("mesh.difference", "Difference");
  const Id x = g.add("mesh.intersection", "Intersection");
  for (const Id& n : {u, d, x}) {
    g.link(a, "mesh", n, "a");
    g.link(b, "mesh", n, "b");
  }
  const Id mu = g.add("mesh.measure", "U");
  const Id md = g.add("mesh.measure", "D");
  const Id mx = g.add("mesh.measure", "X");
  g.link(u, "mesh", mu, "mesh");
  g.link(d, "mesh", md, "mesh");
  g.link(x, "mesh", mx, "mesh");
  CHECK(measured(g, mu, "volume") == Approx(1500.0));
  CHECK(measured(g, md, "volume") == Approx(500.0));
  CHECK(measured(g, mx, "volume") == Approx(500.0));
  const NodeInspection ins = inspect(g, u);
  REQUIRE(ins.ok());
  const auto& m = *std::get<MeshValue>(*ins.find("mesh"));
  const auto topology = mesh::analyse(m);
  CHECK(topology.watertight());
  CHECK(topology.manifold());
  // Deterministic: the same inputs give the same mesh, bit for bit.
  const NodeInspection again = inspect(g, u);
  CHECK(hash_value(*again.find("mesh")) == hash_value(*ins.find("mesh")));
}

TEST_CASE("A mesh that isn't closed can't take part in a boolean", "[proc][mesh_nodes]") {
  mesh::Mesh open;
  const auto a = open.add_vertex({0, 0, 0});
  const auto b = open.add_vertex({1, 0, 0});
  const auto c = open.add_vertex({0, 1, 0});
  open.add_triangle(a, b, c, Id::null());
  Graph g;
  const Id box = g.add("mesh.box", "Box");
  const NodeInspection ins = inspect(g, box);
  REQUIRE(ins.ok());
  const mesh::Mesh& closed = *std::get<MeshValue>(*ins.find("mesh"));
  const auto r = detail::mesh_boolean(closed, open, detail::BooleanOp::Union, Id::null());
  CHECK_FALSE(r.mesh);
  CHECK(r.bad_input == 2);
  const auto r2 = detail::mesh_boolean(open, closed, detail::BooleanOp::Difference, Id::null());
  CHECK(r2.bad_input == 1);
  // Disjoint closed meshes are fine; their intersection is empty.
  mesh::Mesh far = closed;
  for (auto& p : far.positions) p.x += 100.0f;
  const auto r3 = detail::mesh_boolean(closed, far, detail::BooleanOp::Intersection, Id::null());
  REQUIRE(r3.mesh);
  CHECK(r3.mesh->empty());
}

TEST_CASE("Mesh instances copy a template to every frame", "[proc][mesh_nodes]") {
  Graph g;
  const Id box = g.add("mesh.box", "Box", {{"size", Setting(dvec3{2, 2, 2})}});
  const Id ring = g.add("pattern.circular", "Ring", {{"count", Setting(6.0)}});
  const Id start = g.add("frames.point", "Start", {{"position", Setting(dvec3{10, 0, 0})}});
  g.link(start, "frames", ring, "frames");
  const Id copies = g.add("mesh.instances", "Copies");
  g.link(box, "mesh", copies, "template");
  g.link(ring, "frames", copies, "frames");
  const Id m = g.add("mesh.measure", "Size");
  g.link(copies, "mesh", m, "mesh");
  CHECK(measured(g, m, "triangles") == 72.0);
  CHECK(measured(g, m, "volume") == Approx(48.0).epsilon(1e-5));
  // And through the boolean they become one solid.
  const Id u = g.add("mesh.union", "Joined");
  g.link(copies, "mesh", u, "a");
  const Id hub = g.add("mesh.cylinder", "Hub", {{"radius", Setting(11.0)}, {"height", Setting(1.0)}});
  g.link(hub, "mesh", u, "b");
  const NodeInspection ins = inspect(g, u);
  INFO(scenes::codes(ins.diagnostics));
  REQUIRE(ins.ok());
  CHECK(mesh::analyse(*std::get<MeshValue>(*ins.find("mesh"))).watertight());
}

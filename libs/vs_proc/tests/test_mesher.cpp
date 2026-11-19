// Surface nets, the boundary clip, seams and the conversion nodes (M11).

#include <cmath>
#include <numbers>
#include <random>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "scenes.hpp"
#include "vs/eval/engine.hpp"
#include "vs/kernel/operations.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/mesh/tessellate.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/proc/evaluate.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/mesher.hpp"

using namespace vs;
using namespace vs::proc;
using core::dvec3;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

SampledField sampled(const Field& f, const core::dbox3& box, double cell) {
  const auto grid = grid_covering(box, cell);
  REQUIRE(grid);
  return sample(*f, *grid);
}

void require_closed(const mesh::Mesh& m) {
  REQUIRE(m.check().empty());
  const auto t = mesh::analyse(m);
  INFO("open " << t.boundary_edges.size() << ", non-manifold " << t.non_manifold_edges.size()
               << ", inconsistent " << t.inconsistent_edges.size() << ", pinched "
               << t.non_manifold_vertices.size() << ", degenerate "
               << t.degenerate_triangles.size());
  REQUIRE(t.watertight());
  REQUIRE(t.manifold());
}

}  // namespace

TEST_CASE("Surface nets mesh a sphere closed, manifold and on its surface", "[proc][mesher]") {
  const dvec3 c{1.5, -2.25, 3.0};
  const double r = 7.3;
  const auto s = sampled(sphere_field(c, r), core::dbox3::from_min_max({-10, -14, -9}, {13, 10, 15}),
                         0.5);
  const FieldMesh fm = surface_nets(s);
  REQUIRE_FALSE(fm.empty());
  const mesh::Mesh m = to_mesh(fm, core::Id::random());
  require_closed(m);
  CHECK(mesh::analyse(m).shells == 1);
  for (const dvec3& p : fm.positions) REQUIRE(std::abs(length(p - c) - r) < 0.5 * 0.5);
  const double v = 4.0 / 3.0 * std::numbers::pi * r * r * r;
  CHECK_THAT(mesh::enclosed_volume(m), WithinRel(v, 0.02));
}

TEST_CASE("Surface nets stay closed and manifold on any field", "[proc][mesher]") {
  // Random samples: every ambiguous face and cell configuration shows up.
  std::mt19937_64 rng(11);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  for (int round = 0; round < 40; ++round) {
    SampledField s;
    s.grid.origin = {0, 0, 0};
    s.grid.cell = 1.0;
    s.grid.nx = 9;
    s.grid.ny = 8;
    s.grid.nz = 7;
    s.values.resize(s.grid.size());
    for (float& v : s.values) v = u(rng);
    if (round % 4 == 0) {
      // Exact zeros (outside) and touching corners.
      for (std::size_t i = 0; i < s.values.size(); i += 3) s.values[i] = 0.0f;
    }
    INFO("round " << round);
    const mesh::Mesh m = to_mesh(surface_nets(s), core::Id::random());
    require_closed(m);
    CHECK(mesh::enclosed_volume(m) > 0.0);
  }

  // A region filling the grid closes at its edge.
  SampledField full;
  full.grid.nx = full.grid.ny = full.grid.nz = 4;
  full.values.assign(full.grid.size(), -1.0f);
  const mesh::Mesh box = to_mesh(surface_nets(full), core::Id::random());
  require_closed(box);
  CHECK(mesh::analyse(box).shells == 1);

  // Nothing inside: no mesh.
  SampledField none = full;
  none.values.assign(none.grid.size(), 1.0f);
  CHECK(surface_nets(none).empty());
}

TEST_CASE("The same samples always give the same mesh", "[proc][mesher]") {
  const auto f = smooth_union_field(sphere_field({0, 0, 0}, 5), box_field({4, 1, 0}, {6, 3, 8}), 1.5);
  const auto s = sampled(f, core::dbox3::from_min_max({-8, -8, -8}, {10, 8, 8}), 0.4);
  const auto a = surface_nets(s);
  const auto b = surface_nets(s);
  CHECK(a.positions == b.positions);
  CHECK(a.triangles == b.triangles);

  // Through the clip and seams, on pools of any size.
  const auto boundary_solid = kernel::make_box(core::dbox3::from_min_max({-6, -6, -6}, {9, 6, 6}), kSettings);
  REQUIRE(boundary_solid);
  const auto vm = mesh::tessellate(*boundary_solid, {core::SubjectKind::Volume, core::Id::random(), "b"},
                                   {}, kSettings, {});
  REQUIRE(vm.ok());
  const TriangleDistance boundary(vm.mesh, true);
  const core::SubjectRef volume{core::SubjectKind::Volume, core::Id::random(), "V"};
  const auto one = mesh_contents(s, boundary, {}, volume);
  for (const std::size_t n : {1u, 3u, 8u}) {
    eval::ThreadPool pool(n);
    const auto other = mesh_contents(s, boundary, {}, volume, &pool);
    CHECK(other.mesh == one.mesh);
  }
}

TEST_CASE("Clipped contents stay inside a concave boundary", "[proc][mesher]") {
  // An L: the averaged vertices round its inner edge would land outside.
  const auto a = kernel::make_box(core::dbox3::from_min_max({0, 0, 0}, {30, 10, 10}), kSettings);
  const auto b = kernel::make_box(core::dbox3::from_min_max({0, 0, 0}, {10, 30, 10}), kSettings);
  REQUIRE(a);
  REQUIRE(b);
  const auto l = kernel::unite(*a, *b);
  REQUIRE(l);
  const auto vm = mesh::tessellate(*l, {core::SubjectKind::Volume, core::Id::random(), "L"}, {},
                                   kSettings, {});
  REQUIRE(vm.ok());
  const TriangleDistance boundary(vm.mesh, true);
  const core::SubjectRef volume{core::SubjectKind::Volume, core::Id::random(), "L"};
  // Everything: the contents are the boundary itself. A cell that doesn't
  // divide the box puts no grid plane on the inner faces.
  const auto s = sampled(constant_field(-1), kernel::bounding_box(*l), 1.3);
  const auto cm = mesh_contents(s, boundary, {}, volume);
  REQUIRE(cm.ok());
  require_closed(cm.mesh);
  CHECK(cm.moved_vertices > 0);
  const auto contained = validate::check_containment(*l, mesh::view(cm.mesh), volume, kSettings);
  INFO(scenes::codes(contained.diagnostics));
  CHECK(contained.contained);
}

TEST_CASE("field_to_mesh and mesh_to_field convert explicitly", "[proc][mesher][nodes]") {
  Graph g;
  const Id ball = g.add("field.sphere", "Ball", {{"centre", Setting(dvec3{0, 0, 0})}, {"radius", Setting(6.0)}});
  const Id to_mesh_node = g.add("convert.field_to_mesh", "Mesh it", {{"cell_size", Setting(0.5)}});
  const Id to_field = g.add("convert.mesh_to_field", "Back to a field");
  const Id out = g.add("output", "Output");
  g.link(ball, "field", to_mesh_node, "field");
  g.link(to_mesh_node, "mesh", to_field, "mesh");
  g.link(to_field, "field", out, "contents");

  auto inputs = scenes::bare_inputs();
  inputs.bounds = core::dbox3::from_min_max({-10, -10, -10}, {10, 10, 10});
  EvaluateGraphOptions o;
  o.keep_outputs = true;
  const auto ev = evaluate_graph(g, inputs, o);
  INFO(scenes::codes(ev.diagnostics));
  REQUIRE(ev.ok());

  const NodeRun* mesher = ev.find(to_mesh_node);
  REQUIRE(mesher != nullptr);
  REQUIRE(mesher->outputs.size() == 3);
  const auto& m = std::get<MeshValue>(mesher->outputs[0]);
  REQUIRE(m);
  require_closed(*m);
  CHECK(std::get<double>(mesher->outputs[1]) == 41.0 * 41.0 * 41.0);  // the cost
  CHECK(std::get<double>(mesher->outputs[2]) == static_cast<double>(m->triangle_count()));

  const NodeRun* back = ev.find(to_field);
  REQUIRE(back != nullptr);
  CHECK(std::get<double>(back->outputs[1]) == 1.0);  // closed: signed
  const Field& f = *ev.contents;
  std::mt19937_64 rng(5);
  std::uniform_real_distribution<double> u(-12.0, 12.0);
  for (int i = 0; i < 300; ++i) {
    const dvec3 p{u(rng), u(rng), u(rng)};
    REQUIRE_THAT(f->eval(p), WithinAbs(length(p) - 6.0, 0.25));
  }

  // A cell too fine for the grid is infeasible, naming the cell size.
  Graph fine = g;
  fine.find_node(to_mesh_node)->settings["cell_size"] = Setting(0.01);
  const auto bad = evaluate_graph(fine, inputs);
  const auto inf = bad.diagnostics.with_code("proc.node.infeasible");
  REQUIRE(inf.size() == 1);
  REQUIRE(inf.front()->subjects.size() >= 2);
  CHECK(inf.front()->subjects[1].label == "Mesh it.cell_size");
}

TEST_CASE("A procedural volume's seams are stitched in", "[proc][mesher][seam]") {
  // Three procedural boxes in a row, two bonded joints.
  const auto s = scenes::shell_and_ball();
  auto b = scenes::boxes({s.graph, s.graph, s.graph});
  eval::Engine engine(kSettings);
  install(engine);
  eval::EvaluateOptions o;
  o.weld = true;
  const auto ev = engine.evaluate(b.project, o);
  INFO(scenes::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  for (std::size_t i = 0; i < 3; ++i) {
    const FieldContents* fc = field_contents(ev.volumes[i]);
    REQUIRE(fc != nullptr);
    CHECK(fc->stitched.size() == (i == 1 ? 2u : 1u));
    const mesh::Mesh* m = contents_mesh(ev.volumes[i]);
    REQUIRE(m != nullptr);
    require_closed(*m);
  }
  REQUIRE(ev.combined);
  const auto& w = ev.combined->weld;
  CHECK(w.ok());
  CHECK(w.topology.watertight());
  CHECK(w.topology.manifold());
  CHECK(w.internal_triangles > 0);

  // The contents cut back from a joint can't take its seam: a warning on
  // the surface, and the weld fails there.
  Graph cut = s.graph;
  {
    const Id dist = cut.add("query.distance_to_surface", "Near joint",
                            {{"surface", Setting(b.surfaces[0].to_string())}});
    const Id slot = cut.add("field.offset", "Slot", {{"distance", Setting(1.5)}});
    const Id diff = cut.add("field.difference", "Cut");
    cut.link(dist, "distance", slot, "field");
    for (auto& e : cut.edges) {
      if (e.to.node == s.output) e.to = {diff, "a"};
    }
    cut.link(slot, "field", diff, "b");
    cut.link(diff, "field", s.output, "contents");
  }
  std::get<model::ProceduralContents>(b.project.document.volumes[0].contents).graph = to_json(cut);
  const auto ev2 = engine.evaluate(b.project, o);
  const auto uncovered = ev2.diagnostics.with_code("proc.mesh.seam_uncovered");
  REQUIRE(uncovered.size() == 1);
  CHECK(uncovered.front()->severity == core::Severity::Warning);
  CHECK(uncovered.front()->subjects.front().id == b.surfaces[0]);
  CHECK(ev2.volumes[0].status == model::VolumeStatus::Current);
  CHECK_FALSE(ev2.diagnostics.with_code("mesh.weld.nonconforming").empty());
}

TEST_CASE("Seams stitch for any contents covering them, at any cell size", "[proc][mesher][seam]") {
  const int contents = GENERATE(0, 1, 2);
  const double cell = GENERATE(0.7, 1.3, 2.0);
  const int where = GENERATE(0, 1, 2);
  // A 2 mm shell sampled 2 mm apart has no inside samples at all: skip.
  if (contents == 0 && cell > 1.5) return;
  // The patch: centred, the whole face, or small and off to one side.
  const scenes::Patch patch = where == 0   ? scenes::Patch{}
                              : where == 1 ? scenes::Patch{15, 10, 30, 20}
                                           : scenes::Patch{22, 6, 6, 5};
  Graph g;
  const Id out = g.add("output", "Output", {{"cell_size", Setting(cell)}});
  if (contents == 0) {
    const auto sb = scenes::shell_and_ball();
    g = sb.graph;
    g.find_node(sb.output)->settings["cell_size"] = Setting(cell);
  } else if (contents == 1) {
    const Id all = g.add("field.constant", "All", {{"value", Setting(-1.0)}});
    g.link(all, "field", out, "contents");
  } else {
    // A little inside the boundary: the seam is lifted onto the face.
    const Id b = g.add("input.boundary", "Boundary");
    const Id in = g.add("field.offset", "In", {{"distance", Setting(-0.3)}});
    g.link(b, "field", in, "field");
    g.link(in, "field", out, "contents");
  }
  INFO("contents " << contents << ", cell " << cell << ", patch " << where);
  const auto m = scenes::mixed(g, false, patch);
  eval::Engine engine(kSettings);
  install(engine);
  eval::EvaluateOptions o;
  o.weld = true;
  const auto ev = engine.evaluate(m.project, o);
  INFO(scenes::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  const FieldContents* fc = field_contents(*ev.find(m.procedural));
  REQUIRE(fc != nullptr);
  CHECK(fc->stitched.size() == 1);
  if (contents == 1) {
    // Filled: the whole box, less the thin inset and the box's edges,
    // which surface nets cut off about a cell and a half wide (about
    // 0.75 cell^2 a millimetre of edge, 360 mm of them).
    const mesh::Mesh* pm = contents_mesh(*ev.find(m.procedural));
    REQUIRE(pm != nullptr);
    const double lost = 40.0 * 30.0 * 20.0 - mesh::enclosed_volume(*pm);
    CHECK(lost > 0.0);
    CHECK(lost < 360.0 * 0.75 * cell * cell + 5200.0 * kContentsInset * 2);
  }
  REQUIRE(ev.combined);
  CHECK(ev.combined->weld.topology.watertight());
  CHECK(ev.combined->weld.topology.manifold());
}


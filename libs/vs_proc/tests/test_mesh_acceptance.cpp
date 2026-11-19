// M11's acceptance items (Implementation Plan, M11):
//   1. A procedural volume next to a parametric volume welds into one
//      watertight mesh across their shared connection surface.
//   2. Containment holds for random fields clipped to random boundaries.
//   3. Mesh output hashes are identical across runs and thread counts.

#include <cmath>
#include <random>
#include <string>

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
#include "vs/proc/generator.hpp"
#include "vs/proc/mesher.hpp"
#include "vs/validate/containment.hpp"

using namespace vs;
using namespace vs::proc;
using core::dvec3;
using Catch::Matchers::WithinRel;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

eval::EngineOptions threads(std::size_t n) {
  eval::EngineOptions o;
  o.threads = n;
  return o;
}

core::Hash256 mesh_hash(const mesh::Mesh& m) {
  core::Hasher h;
  for (const auto& p : m.positions) h.add(p);
  for (const auto& id : m.seam_vertices) h.add(id);
  for (const auto i : m.indices) h.add(i);
  for (const auto& id : m.triangle_volumes) h.add(id);
  return h.finalize();
}

}  // namespace

TEST_CASE("Acceptance 1: a procedural volume welds to a parametric one across their surface",
          "[proc][mesher][acceptance]") {
  const bool flange = GENERATE(false, true);
  INFO((flange ? "bolted flange" : "bonded"));
  const auto m = scenes::mixed(scenes::shell_and_ball().graph, flange);
  eval::Engine engine(kSettings, threads(4));
  install(engine);
  eval::EvaluateOptions o;
  o.weld = true;
  const auto ev = engine.evaluate(m.project, o);
  INFO(scenes::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  CHECK(ev.diagnostics.with_code("validate.containment").empty());

  const auto* p = ev.find(m.procedural);
  const auto* q = ev.find(m.parametric);
  REQUIRE(p != nullptr);
  REQUIRE(q != nullptr);
  CHECK(p->status == model::VolumeStatus::Current);
  CHECK(q->status == model::VolumeStatus::Current);
  const FieldContents* fc = field_contents(*p);
  REQUIRE(fc != nullptr);
  REQUIRE(fc->stitched.size() == 1);
  CHECK(fc->stitched.front() == m.surface);

  REQUIRE(ev.combined);
  const auto& c = *ev.combined;
  CHECK(c.ok());
  REQUIRE(c.volumes.size() == 2);
  for (const auto& vt : c.volumes) {
    REQUIRE(vt.seams.size() == 1);
    CHECK(vt.seams.front() == m.surface);
  }
  const auto& w = c.weld;
  CHECK(w.ok());
  CHECK(w.topology.watertight());
  CHECK(w.topology.manifold());
  // Q and P's outer skin are one shell; P's hollow and its ball are two more.
  CHECK(w.topology.shells == 3);
  CHECK(w.internal_triangles > 0);
  CHECK(w.mesh.volumes().size() == 2);
  // Welding removes only the shared patch, so the volume is the two parts'.
  CHECK_THAT(mesh::enclosed_volume(w.mesh),
             WithinRel(mesh::enclosed_volume(c.volumes[0].mesh) +
                           mesh::enclosed_volume(c.volumes[1].mesh),
                       1e-6));
  CHECK_THAT(mesh::enclosed_volume(c.volumes[1].mesh), WithinRel(40.0 * 30.0 * 20.0, 1e-6));
}

TEST_CASE("Acceptance 2: containment holds for random fields clipped to random boundaries",
          "[proc][mesher][acceptance]") {
  std::mt19937_64 rng(2026);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  const auto in = [&](double lo, double hi) { return lo + (hi - lo) * u(rng); };
  const auto box_at = [&](const dvec3& lo, const dvec3& size) {
    auto b = kernel::make_box(core::dbox3::from_min_max(lo, lo + size), kSettings);
    REQUIRE(b);
    return *b;
  };

  for (int round = 0; round < 24; ++round) {
    // A boundary: a box, a cylinder on a random axis, a sphere, an L, or a
    // box with a round hole through it (concave and curved).
    const dvec3 lo{in(-50, 50), in(-50, 50), in(-50, 50)};
    const dvec3 size{in(10, 40), in(10, 40), in(10, 40)};
    kernel::Solid boundary;
    std::string kind;
    switch (round % 5) {
      case 0:
        kind = "box";
        boundary = box_at(lo, size);
        break;
      case 1: {
        kind = "cylinder";
        const dvec3 axis = normalize(dvec3{in(-1, 1), in(-1, 1), in(0.2, 1)});
        auto c = kernel::make_cylinder({lo, axis}, in(5, 15), in(10, 30), kSettings);
        REQUIRE(c);
        boundary = *c;
        break;
      }
      case 2: {
        kind = "sphere";
        auto s = kernel::make_sphere(lo, in(6, 18), kSettings);
        REQUIRE(s);
        boundary = *s;
        break;
      }
      case 3: {
        kind = "L";
        const double t = in(4, 8);
        auto l = kernel::unite(box_at(lo, {size.x, t, size.z}), box_at(lo, {t, size.y, size.z}));
        REQUIRE(l);
        boundary = *l;
        break;
      }
      default: {
        kind = "box with a hole";
        const double r = std::min(size.x, size.y) * in(0.15, 0.35);
        auto hole = kernel::make_cylinder(
            {lo + dvec3{size.x / 2, size.y / 2, -1}, {0, 0, 1}}, r, size.z + 2, kSettings);
        REQUIRE(hole);
        auto b = kernel::cut(box_at(lo, size), *hole);
        REQUIRE(b);
        boundary = *b;
        break;
      }
    }
    const core::dbox3 bounds = kernel::bounding_box(boundary);
    mesh::TessellationOptions to;
    to.params.deflection = kInputDeflection;
    const core::SubjectRef volume{core::SubjectKind::Volume, core::Id::random(), kind};
    const auto vm = mesh::tessellate(boundary, volume, {}, kSettings, to);
    REQUIRE(vm.ok());
    auto distance = std::make_shared<const TriangleDistance>(vm.mesh, true);

    // A field: everything, the boundary grown or shelled, or a few random
    // blobs, some reaching out of it.
    Field f;
    switch (round % 4) {
      case 0:
        f = constant_field(-1.0);
        break;
      case 1:
        f = offset_field(mesh_signed_field(distance), in(0.5, 3));
        break;
      case 2:
        f = shell_field(mesh_signed_field(distance), in(1.5, 4));
        break;
      default: {
        const dvec3 c = (bounds.min + bounds.max) * 0.5;
        const double r = length(bounds.size()) * 0.3;
        f = sphere_field(c + dvec3{in(-r, r), in(-r, r), in(-r, r)}, in(0.4, 1.0) * r);
        for (int k = 0; k < 3; ++k) {
          f = smooth_union_field(
              f, box_field(c + dvec3{in(-r, r), in(-r, r), in(-r, r)}, {in(2, r), in(2, r), in(2, r)}),
              in(0, 2));
        }
        break;
      }
    }
    const double longest = std::max({bounds.size().x, bounds.size().y, bounds.size().z});
    const double cell = longest / in(16, 40);
    INFO("round " << round << ": " << kind << ", cell " << cell);
    const auto grid = grid_covering(bounds, cell);
    REQUIRE(grid);
    const auto samples = sample(*f, *grid);
    const auto cm = mesh_contents(samples, *distance, {}, volume);
    INFO(scenes::codes(cm.diagnostics));
    REQUIRE(cm.ok());
    REQUIRE_FALSE(cm.mesh.empty());
    const auto topology = mesh::analyse(cm.mesh);
    CHECK(topology.watertight());
    CHECK(topology.manifold());
    const auto contained = validate::check_containment(boundary, mesh::view(cm.mesh), volume, kSettings);
    INFO(scenes::codes(contained.diagnostics));
    CHECK(contained.contained);
  }
}

TEST_CASE("Acceptance 3: mesh output hashes are identical across runs and thread counts",
          "[proc][mesher][acceptance]") {
  // Procedural volumes next to each other and to a parametric one, one
  // seeded at random, welded.
  auto s = scenes::shell_and_ball();
  Graph seeded = s.graph;
  {
    const Id rnd = seeded.add("math.random", "Dice", {{"min", Setting(4.0)}, {"max", Setting(8.0)}});
    seeded.link(rnd, "value", s.ball, "radius");
  }
  auto b = scenes::boxes({s.graph, seeded, seeded});
  auto mixed = scenes::mixed(seeded, true);

  for (const model::Project* project : {&b.project, &mixed.project}) {
    std::vector<core::Hash256> hashes;
    std::vector<std::vector<core::Hash256>> meshes;
    for (const std::size_t n : {1u, 4u, 16u, 4u}) {
      eval::Engine engine(kSettings, threads(n));
      install(engine);
      eval::EvaluateOptions o;
      o.weld = true;
      const auto ev = engine.evaluate(*project, o);
      INFO(scenes::codes(ev.diagnostics));
      REQUIRE(ev.ok());
      REQUIRE(ev.combined);
      CHECK(ev.combined->weld.topology.watertight());
      hashes.push_back(ev.output_hash());  // includes the welded mesh
      meshes.emplace_back();
      for (const auto& r : ev.volumes) {
        if (const mesh::Mesh* m = contents_mesh(r)) meshes.back().push_back(mesh_hash(*m));
      }
      meshes.back().push_back(mesh_hash(ev.combined->weld.mesh));
    }
    for (std::size_t k = 1; k < hashes.size(); ++k) {
      CHECK(hashes[k] == hashes[0]);
      CHECK(meshes[k] == meshes[0]);
    }

    // The engine's own check: generate each volume twice and compare.
    eval::EngineOptions o = threads(4);
    o.check_determinism = true;
    eval::Engine engine(kSettings, o);
    install(engine);
    eval::EvaluateOptions eo;
    eo.weld = true;
    const auto ev = engine.evaluate(*project, eo);
    CHECK(ev.diagnostics.with_code("eval.nondeterministic").empty());
    CHECK(ev.output_hash() == hashes[0]);
  }
}

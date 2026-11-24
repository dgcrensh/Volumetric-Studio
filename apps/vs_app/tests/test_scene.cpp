// The scene: what the viewport draws for each sample project.

#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "outline.hpp"
#include "samples.hpp"
#include "scene.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/queries.hpp"

using namespace vs;
using app::ItemRole;
using Catch::Matchers::WithinAbs;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

bool unit(const core::fvec3& n) { return std::abs(length(n) - 1.0f) < 1e-4f; }

void check_mesh(const app::DisplayMesh& m) {
  REQUIRE(m.normals.size() == m.positions.size());
  REQUIRE(m.indices.size() % 3 == 0);
  for (const auto i : m.indices) REQUIRE(i < m.positions.size());
  for (const auto& n : m.normals) REQUIRE(unit(n));
}

bool box_contains(const core::fbox3& outer, const core::fbox3& inner, float tol) {
  return outer.contains(inner, tol);
}

}  // namespace

TEST_CASE("A box solid becomes 12 flat-shaded triangles with 12 edges", "[app][scene]") {
  const auto box = kernel::make_box(core::dbox3::from_min_max({0, 0, 0}, {10, 20, 30}), kSettings);
  REQUIRE(box.ok());
  const auto m = app::display_mesh(*box, kSettings);
  REQUIRE(m.ok());
  check_mesh(*m);
  CHECK(m->triangle_count() == 12);
  CHECK(m->positions.size() == 24);  // four corners per face: the creases split them
  CHECK(m->edges.size() == 12);
  for (const auto& n : m->normals) {
    // Every normal is along an axis.
    const float big = std::max({std::abs(n.x), std::abs(n.y), std::abs(n.z)});
    CHECK_THAT(big, WithinAbs(1.0, 1e-6));
  }
  const auto b = m->bounds();
  CHECK_THAT(b.max.z, WithinAbs(30.0, 1e-5));
  // Triangles face out: the signed volume is positive.
  double volume = 0;
  for (std::size_t t = 0; t < m->triangle_count(); ++t) {
    const auto a = core::to_fp64(m->positions[m->indices[3 * t]]);
    const auto bb = core::to_fp64(m->positions[m->indices[3 * t + 1]]);
    const auto c = core::to_fp64(m->positions[m->indices[3 * t + 2]]);
    volume += dot(a, cross(bb, c)) / 6.0;
  }
  CHECK_THAT(volume, WithinAbs(6000.0, 1e-3));
}

TEST_CASE("A sphere is shaded smoothly: no vertex is split", "[app][scene]") {
  const auto ball = kernel::make_sphere({0, 0, 0}, 10.0, kSettings);
  REQUIRE(ball.ok());
  const auto m = app::display_mesh(*ball, kSettings);
  REQUIRE(m.ok());
  check_mesh(*m);
  CHECK(m->triangle_count() > 100);
  // Each normal points away from the centre.
  for (std::size_t i = 0; i < m->positions.size(); ++i) {
    const auto radial = normalize(m->positions[i]);
    CHECK(dot(radial, m->normals[i]) > 0.95f);
  }
}

TEST_CASE("Mesh contents split vertices only at creases", "[app][scene]") {
  mesh::Mesh flat;
  const auto a = flat.add_vertex({0, 0, 0}), b = flat.add_vertex({1, 0, 0});
  const auto c = flat.add_vertex({1, 1, 0}), d = flat.add_vertex({0, 1, 0});
  flat.add_triangle(a, b, c, core::Id{1, 1});
  flat.add_triangle(a, c, d, core::Id{1, 1});
  const auto m1 = app::display_mesh(flat);
  check_mesh(m1);
  CHECK(m1.positions.size() == 4);

  mesh::Mesh folded;
  const auto e = folded.add_vertex({0, 0, 0}), f = folded.add_vertex({1, 0, 0});
  const auto g = folded.add_vertex({1, 1, 0}), h = folded.add_vertex({1, 0, 1});
  folded.add_triangle(e, f, g, core::Id{1, 1});
  folded.add_triangle(e, h, f, core::Id{1, 1});  // at 90 degrees along e-f
  const auto m2 = app::display_mesh(folded);
  check_mesh(m2);
  CHECK(m2.positions.size() == 6);
}

TEST_CASE("Every sample project draws its boundaries, contents and connection surfaces",
          "[app][scene][acceptance]") {
  const auto paths = samples::projects();
  REQUIRE(paths.size() >= 9);
  for (const auto& path : paths) {
    const std::string name = path.filename().string();
    INFO(name);
    auto loaded = model::load_project(path, false);
    REQUIRE(loaded.ok());
    const eval::Evaluation ev = samples::evaluate(*loaded);
    const app::Scene scene = app::build_scene(ev, kSettings);
    INFO(samples::codes(ev.diagnostics) + samples::codes(scene.diagnostics));
    CHECK(scene.diagnostics.empty());

    if (!ev.checked) {
      // A document that breaks the authoring rules (loop.vsproj) builds
      // nothing: the diagnostics list is what the app shows.
      CHECK(ev.diagnostics.has_errors());
      CHECK(scene.items.empty());
      continue;
    }
    REQUIRE(!scene.items.empty());
    for (const auto& item : scene.items) check_mesh(item.mesh);

    for (const auto& volume : ev.document.volumes) {
      INFO(volume.name);
      const auto* vb = ev.build.find_volume(volume.id);
      REQUIRE(vb != nullptr);
      if (!vb->boundary.is_empty()) {
        const auto* boundary = scene.find(volume.id, ItemRole::Boundary);
        REQUIRE(boundary != nullptr);
        CHECK(boundary->mesh.triangle_count() >= 12);
        CHECK(boundary->transparency > 0.5f);
        // Drawn where the kernel says it is.
        const auto exact = kernel::bounding_box(vb->boundary);
        const auto drawn = boundary->mesh.bounds();
        CHECK_THAT(drawn.min.x, WithinAbs(exact.min.x, 1e-3));
        CHECK_THAT(drawn.max.y, WithinAbs(exact.max.y, 1e-3));
        CHECK_THAT(drawn.max.z, WithinAbs(exact.max.z, 1e-3));
      }
      const auto* r = ev.find(volume.id);
      REQUIRE(r != nullptr);
      const bool shows = r->contents && (r->contents->mesh || !r->contents->solid.is_empty());
      const auto* contents = scene.find(volume.id, ItemRole::Contents);
      CHECK((contents != nullptr) == shows);
      if (contents != nullptr) {
        CHECK(contents->mesh.triangle_count() > 0);
        CHECK(contents->stale == r->stale_contents);
        if (contents->stale) CHECK(contents->color == app::kStaleColor);
        // Contents are drawn inside their boundary (generated meshes are
        // contained; imported files that aren't fail containment).
        const auto* boundary = scene.find(volume.id, ItemRole::Boundary);
        if (boundary != nullptr && r->status == model::VolumeStatus::Current) {
          CHECK(box_contains(boundary->mesh.bounds(), contents->mesh.bounds(), 1e-3f));
        }
      }
    }
    for (const auto& surface : ev.document.connections) {
      INFO(surface.name);
      const auto* cb = ev.build.find_connection(surface.id);
      REQUIRE(cb != nullptr);
      const auto* item = scene.find(surface.id, ItemRole::ConnectionSurface);
      CHECK((item != nullptr) == cb->ok);
      if (item != nullptr) {
        CHECK(item->color == app::connection_color(surface.type()));
        CHECK(item->mesh.triangle_count() >= 2);
      }
    }
    CHECK(box_contains(scene.bounds, scene.items.front().mesh.bounds(), 0.0f));
  }
}

TEST_CASE("Connection surface types have distinct colours", "[app][scene]") {
  const auto bonded = app::connection_color(model::ConnectionType::Bonded);
  const auto clearance = app::connection_color(model::ConnectionType::Clearance);
  const auto flange = app::connection_color(model::ConnectionType::BoltedFlange);
  CHECK(bonded != clearance);
  CHECK(bonded != flange);
  CHECK(clearance != flange);
  CHECK(app::volume_color(0) != app::volume_color(1));
}

TEST_CASE("An imported STL draws as static mesh contents", "[app][scene][acceptance]") {
  const auto project = samples::static_mesh();
  const auto ev = samples::evaluate(project);
  INFO(samples::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  const auto scene = app::build_scene(ev, kSettings);
  const auto& v = ev.document.volumes.at(0);
  const auto* contents = scene.find(v.id, ItemRole::Contents);
  REQUIRE(contents != nullptr);
  CHECK(contents->mesh.triangle_count() == 20);
  CHECK(scene.find(v.id, ItemRole::Boundary) != nullptr);
}

TEST_CASE("Volumes are coloured by their place in the volume tree, as the tree's rows (H5)",
          "[app][scene]") {
  const auto project = samples::load("nested.vsproj");
  const eval::Evaluation ev = samples::evaluate(project);
  REQUIRE(ev.checked);
  const auto order = app::volume_tree_order(ev.document);
  const auto rows = app::volume_rows(ev);
  REQUIRE(order.size() == ev.document.volumes.size());
  REQUIRE(rows.size() == order.size());
  // The nested sample's tree order isn't its document order, or this
  // would test nothing.
  bool differs = false;
  for (std::size_t k = 0; k < order.size(); ++k) differs = differs || order[k].index != k;
  CHECK(differs);

  const app::Scene scene = app::build_scene(ev, kSettings);
  std::size_t drawn = 0;
  for (std::size_t k = 0; k < order.size(); ++k) {
    const auto& volume = ev.document.volumes[order[k].index];
    INFO(volume.name);
    CHECK(rows[k].id == volume.id);
    if (volume.keep_out) continue;
    const auto* boundary = scene.find(volume.id, ItemRole::Boundary);
    if (boundary == nullptr) continue;
    ++drawn;
    CHECK(boundary->color == app::volume_color(k));
  }
  CHECK(drawn >= 4);
}

TEST_CASE("A sketch's Bezier is drawn as a fine polyline on its plane", "[app][scene][bezier]") {
  vs::sketch::Sketch s;
  s.add_bezier(vs::core::dvec2{0, 0}, vs::core::dvec2{0, 30}, vs::core::dvec2{40, 30},
               vs::core::dvec2{40, 0});
  const auto lines = vs::app::sketch_lines(s);
  REQUIRE(lines.size() == 1);
  CHECK(lines.front().size() > 16);
  CHECK(lines.front().front().x == 0.0f);
  CHECK(lines.front().back().x == 40.0f);
}

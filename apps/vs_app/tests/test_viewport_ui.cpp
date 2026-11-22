// The viewport's picking, handles and repaints (H7), on a real display.

#include <QApplication>
#include <QMouseEvent>

#include <catch2/catch_test_macros.hpp>

#include "viewport.hpp"
#include "wait.hpp"

using namespace vs;
using core::Id;

namespace {

// A flat n x n grid of squares on z = `z`, over [0, size]^2, as one B-rep
// item whose two faces are its lower and upper halves (in y): triangles
// row by row, so each face is a contiguous range.
app::SceneItem grid_item(const Id& subject, int n, float size, float z) {
  app::SceneItem item;
  item.role = app::ItemRole::Boundary;
  item.subject = subject;
  item.volume = subject;
  item.color = {0.5f, 0.6f, 0.7f};
  auto& m = item.mesh;
  for (int j = 0; j <= n; ++j) {
    for (int i = 0; i <= n; ++i) {
      m.positions.push_back({size * static_cast<float>(i) / static_cast<float>(n),
                             size * static_cast<float>(j) / static_cast<float>(n), z});
      m.normals.push_back({0.0f, 0.0f, 1.0f});
    }
  }
  const auto at = [n](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
  for (int j = 0; j < n; ++j) {
    for (int i = 0; i < n; ++i) {
      m.indices.insert(m.indices.end(), {at(i, j), at(i + 1, j), at(i + 1, j + 1)});
      m.indices.insert(m.indices.end(), {at(i, j), at(i + 1, j + 1), at(i, j + 1)});
    }
  }
  const auto half = static_cast<std::uint32_t>(m.triangle_count() / 2);
  m.faces.push_back({0, half, {}});
  m.faces.push_back({half, static_cast<std::uint32_t>(m.triangle_count()) - half, {}});
  return item;
}

app::Scene scene_of(std::vector<app::SceneItem> items) {
  app::Scene s;
  s.items = std::move(items);
  s.bounds = s.items.front().mesh.bounds();
  for (const auto& item : s.items) {
    const auto b = item.mesh.bounds();
    s.bounds.min = {std::min(s.bounds.min.x, b.min.x), std::min(s.bounds.min.y, b.min.y),
                    std::min(s.bounds.min.z, b.min.z)};
    s.bounds.max = {std::max(s.bounds.max.x, b.max.x), std::max(s.bounds.max.y, b.max.y),
                    std::max(s.bounds.max.z, b.max.z)};
  }
  return s;
}

// Shows `scene` from the top and waits for it to be drawn.
void show_top(app::Viewport& v, const app::Scene& scene) {
  int frames = 0;
  auto c = QObject::connect(&v, &app::Viewport::frameRendered, [&](double) { ++frames; });
  v.show_scene(scene, nullptr, true);
  v.set_view(app::StandardView::Top);
  REQUIRE(samples::wait_until([&] { return frames > 0 && !v.loading(); }, 10000));
  QObject::disconnect(c);
}

}  // namespace

TEST_CASE("Faces of a B-rep over 60,000 triangles can be picked", "[app][gui][viewport]") {
  app::Viewport v;
  v.resize(800, 600);
  v.show();
  REQUIRE(v.has_gpu());
  const Id id = Id::random();
  const auto item = grid_item(id, 200, 100.0f, 0.0f);  // 80,000 triangles: two parts
  REQUIRE(item.mesh.triangle_count() > 60000);
  show_top(v, scene_of({item}));
  v.set_pick_mode(app::PickMode::Face, {{id, app::ItemRole::Boundary}});
  // Pick mode shows the item again, its second part a frame later.
  REQUIRE(samples::wait_until([&] { return !v.loading(); }, 10000));
  // The lower face lies wholly in the first part, the upper one in both.
  for (const auto& [y, face] : {std::pair{10.0, 0u}, std::pair{60.0, 1u}, std::pair{95.0, 1u}}) {
    INFO("y " << y);
    const auto at = v.project({50.0, y, 0.0});
    REQUIRE(at);
    const auto part = v.pick_at(*at);
    REQUIRE(part);
    CHECK(part->subject == id);
    CHECK(part->kind == app::PickedPart::Kind::Face);
    CHECK(part->index == face);
  }
}

TEST_CASE("Picking in a new scene starts from the front again", "[app][gui][viewport]") {
  app::Viewport v;
  v.resize(800, 600);
  v.show();
  REQUIRE(v.has_gpu());
  const Id below = Id::random(), above = Id::random();
  const auto scene = scene_of({grid_item(below, 4, 100.0f, 0.0f), grid_item(above, 4, 100.0f, 10.0f)});
  show_top(v, scene);
  v.set_pick_mode(app::PickMode::Face, {{below, app::ItemRole::Boundary}, {above, app::ItemRole::Boundary}});
  const auto at = v.project({50.0, 30.0, 10.0});
  REQUIRE(at);
  auto part = v.pick_at(*at);
  REQUIRE(part);
  CHECK(part->subject == above);  // the nearest
  // A new scene (the next evaluation, same view): clicking in the same
  // place picks the front again, not what lay behind the last pick.
  int frames = 0;
  QObject::connect(&v, &app::Viewport::frameRendered, [&](double) { ++frames; });
  v.show_scene(scene, nullptr, false);
  REQUIRE(samples::wait_until([&] { return frames > 0 && !v.loading(); }, 10000));
  part = v.pick_at(*at);
  REQUIRE(part);
  CHECK(part->subject == above);
  part = v.pick_at(*at);  // again in place: what lies behind
  REQUIRE(part);
  CHECK(part->subject == below);
}

TEST_CASE("A handle behind the camera is neither drawn on screen nor hit", "[app][gui][viewport]") {
  app::Viewport v;
  v.resize(800, 600);
  v.show();
  REQUIRE(v.has_gpu());
  show_top(v, scene_of({grid_item(Id::random(), 4, 100.0f, 0.0f)}));
  // The camera looks down from above the grid; this point is far behind it.
  const core::dvec3 behind{50.0, 50.0, 1.0e6};
  CHECK_FALSE(v.project(behind));
  v.set_handles({{behind, {0.0, 0.0, 1.0}}});
  CHECK_FALSE(v.handle_at(QPointF(400.0, 300.0)));
  // One on the grid is.
  const core::dvec3 on{50.0, 50.0, 0.0};
  v.set_handles({{on, {1.0, 0.0, 0.0}}});
  const auto at = v.project(on);
  REQUIRE(at);
  CHECK(v.handle_at(*at) == std::optional<std::size_t>{0});
}

TEST_CASE("Hovering repaints without redrawing the whole view", "[app][gui][viewport]") {
  app::Viewport v;
  v.resize(800, 600);
  v.show();
  REQUIRE(v.has_gpu());
  const auto scene = scene_of({grid_item(Id::random(), 50, 100.0f, 0.0f)});
  show_top(v, scene);
  samples::wait_until([] { return false; }, 200);
  const std::size_t full = v.full_redraws();
  int frames = 0;
  QObject::connect(&v, &app::Viewport::frameRendered, [&](double) { ++frames; });
  for (int i = 0; i < 20; ++i) {
    const QPointF p(200.0 + 20.0 * i, 300.0);
    QMouseEvent move(QEvent::MouseMove, p, v.mapToGlobal(p), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&v, &move);
    samples::wait_until([] { return false; }, 20);
  }
  CHECK(frames > 0);
  CHECK(v.full_redraws() == full);
  // A new scene is drawn in full.
  frames = 0;
  v.show_scene(scene, nullptr, false);
  REQUIRE(samples::wait_until([&] { return frames > 0; }, 10000));
  CHECK(v.full_redraws() == full + 1);
}

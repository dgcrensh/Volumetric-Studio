// L6: views and hidden lines (plan docs/plans/L6-drawings.md, "Projection").

#include <algorithm>
#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "vs/draw/view.hpp"
#include "vs/kernel/primitives.hpp"

using namespace vs::draw;
using vs::core::dbox3;
using vs::core::dvec2;
using vs::core::dvec3;
using vs::core::Id;
using vs::core::ProductSettings;
using Catch::Matchers::WithinAbs;

namespace {

const ProductSettings kSettings = ProductSettings::defaults();

vs::kernel::Solid box(dvec3 lo, dvec3 hi) {
  auto b = vs::kernel::make_box(dbox3::from_min_max(lo, hi), kSettings);
  REQUIRE(b);
  return *b;
}

std::size_t count(const ProjectedView& v, LineKind kind) {
  return static_cast<std::size_t>(std::count_if(
      v.lines.begin(), v.lines.end(), [&](const ViewLine& l) { return l.kind == kind; }));
}

double length(const ViewLine& l) {
  double s = 0;
  for (std::size_t i = 0; i + 1 < l.points.size(); ++i) {
    s += vs::core::length(l.points[i + 1] - l.points[i]);
  }
  return s;
}

double total(const ProjectedView& v, LineKind kind) {
  double s = 0;
  for (const auto& l : v.lines) {
    if (l.kind == kind) s += length(l);
  }
  return s;
}

// A closed box as a mesh (two triangles a side, outward).
vs::mesh::Mesh box_mesh(dvec3 lo, dvec3 hi, const Id& volume) {
  vs::mesh::Mesh m;
  for (int k = 0; k < 8; ++k) {
    m.add_vertex({static_cast<float>(k & 1 ? hi.x : lo.x), static_cast<float>(k & 2 ? hi.y : lo.y),
                  static_cast<float>(k & 4 ? hi.z : lo.z)});
  }
  const std::uint32_t q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                 {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (const auto& f : q) {
    m.add_triangle(f[0], f[1], f[2], volume);
    m.add_triangle(f[0], f[2], f[3], volume);
  }
  return m;
}

}  // namespace

TEST_CASE("View frames are right-handed and look the right way", "[draw][view]") {
  using vs::model::ViewDirection;
  for (auto d : {ViewDirection::Front, ViewDirection::Back, ViewDirection::Top,
                 ViewDirection::Bottom, ViewDirection::Left, ViewDirection::Right,
                 ViewDirection::Isometric}) {
    const ViewFrame f = view_frame(d);
    const dvec3 c = cross(f.right, f.up);
    CHECK_THAT(c.x - f.toward.x, WithinAbs(0, 1e-12));
    CHECK_THAT(c.y - f.toward.y, WithinAbs(0, 1e-12));
    CHECK_THAT(c.z - f.toward.z, WithinAbs(0, 1e-12));
    CHECK_THAT(vs::core::length(f.up), WithinAbs(1, 1e-12));
  }
  // Front: x right, z up, the viewer at -y.
  const ViewFrame front = view_frame(ViewDirection::Front);
  CHECK(front.project({1, 2, 3}) == dvec2{1, 3});
  CHECK(front.depth({0, -5, 0}) > front.depth({0, 5, 0}));
  // Isometric: +x down and right, +y up and right, +z straight up.
  const ViewFrame iso = view_frame(ViewDirection::Isometric);
  CHECK(iso.project({1, 0, 0}).x > 0);
  CHECK(iso.project({1, 0, 0}).y < 0);
  CHECK(iso.project({0, 1, 0}).x > 0);
  CHECK(iso.project({0, 1, 0}).y > 0);
  CHECK_THAT(iso.project({0, 0, 1}).x, WithinAbs(0, 1e-15));
}

TEST_CASE("A box seen from the front is its outline", "[draw][view]") {
  ViewProjector p(view_frame(vs::model::ViewDirection::Front), {}, kSettings);
  const Id src = Id::random();
  p.add_contents(src, box({0, 0, 0}, {60, 40, 10}));
  const auto v = p.run(true);
  CHECK(v.diagnostics.empty());
  // Every edge projects onto the 60 x 10 rectangle: the front face's four
  // are visible; the back face's are on them (dropped), the depth edges are
  // points.
  CHECK_THAT(total(v, LineKind::Visible), WithinAbs(2 * (60 + 10), 1e-6));
  CHECK(count(v, LineKind::Hidden) == 0);
  for (const auto& l : v.lines) {
    CHECK(l.source == src);
    for (const auto& q : l.points) {
      CHECK(q.x >= -1e-9);
      CHECK(q.x <= 60 + 1e-9);
      CHECK(q.y >= -1e-9);
      CHECK(q.y <= 10 + 1e-9);
    }
  }
}

TEST_CASE("A box behind another is hidden where it's covered", "[draw][view]") {
  // Seen from the top, a tall 20 x 20 box hides the middle of a flat 60 x 10
  // one below it.
  ViewProjector p(view_frame(vs::model::ViewDirection::Top), {}, kSettings);
  const Id low = Id::random();
  const Id high = Id::random();
  p.add_contents(low, box({0, 0, 0}, {60, 10, 5}));
  p.add_contents(high, box({20, -5, 0}, {40, 15, 30}));
  const auto with_hidden = p.run(true);
  const auto without = p.run(false);
  CHECK(count(without, LineKind::Hidden) == 0);
  double low_visible = 0, low_hidden = 0;
  for (const auto& l : with_hidden.lines) {
    if (l.source != low) continue;
    (l.kind == LineKind::Visible ? low_visible : low_hidden) += length(l);
  }
  // The low box's outline is 2 * (60 + 10) = 140; 2 * 20 of it is under
  // the tall box, within a sample step at each end.
  CHECK_THAT(low_hidden, WithinAbs(40, 0.25));
  CHECK_THAT(low_visible, WithinAbs(100, 0.25));
  CHECK_THAT(total(with_hidden, LineKind::Visible) - low_visible, WithinAbs(80, 1e-6));
}

TEST_CASE("A cylinder shows its silhouettes; its seam is never drawn", "[draw][view]") {
  auto c = vs::kernel::make_cylinder({{0, 0, 0}, {0, 0, 1}}, 10, 30, kSettings);
  REQUIRE(c);
  // Side on: a 20 x 30 rectangle, its sides the silhouettes.
  for (auto dir : {vs::model::ViewDirection::Front, vs::model::ViewDirection::Right,
                   vs::model::ViewDirection::Left}) {
    ViewProjector p(view_frame(dir), {}, kSettings);
    p.add_contents(Id::random(), *c);
    const auto v = p.run(true);
    INFO("direction " << vs::model::to_string(dir));
    double xmin = 1e9, xmax = -1e9;
    for (const auto& l : v.lines) {
      for (const auto& q : l.points) {
        xmin = std::min(xmin, q.x);
        xmax = std::max(xmax, q.x);
      }
    }
    CHECK_THAT(xmin, WithinAbs(-10, 0.05));
    CHECK_THAT(xmax, WithinAbs(10, 0.05));
    // Two circles seen edge-on (2 x 20) and two silhouettes (2 x 30).
    CHECK_THAT(total(v, LineKind::Visible), WithinAbs(2 * 20 + 2 * 30, 0.2));
    CHECK(count(v, LineKind::Hidden) == 0);
  }
  // End on: one circle; the bottom circle is under it, the seam isn't drawn.
  ViewProjector top(view_frame(vs::model::ViewDirection::Top), {}, kSettings);
  top.add_contents(Id::random(), *c);
  const auto v = top.run(true);
  CHECK_THAT(total(v, LineKind::Visible), WithinAbs(2 * M_PI * 10, 0.1));
  CHECK(count(v, LineKind::Hidden) == 0);
}

TEST_CASE("Outlines are drawn whole and hide nothing", "[draw][view]") {
  ViewProjector p(view_frame(vs::model::ViewDirection::Top), {}, kSettings);
  const Id envelope = Id::random();
  const Id inside = Id::random();
  p.add_outline(envelope, box({0, 0, 0}, {50, 50, 50}));
  p.add_contents(inside, box({10, 10, 10}, {20, 20, 20}));
  const auto v = p.run(true);
  // 12 edges of the envelope: from above, 4 sides of 50 (and the vertical
  // edges as points, dropped).
  double outline = 0;
  for (const auto& l : v.lines) {
    if (l.kind == LineKind::Outline) {
      CHECK(l.source == envelope);
      outline += length(l);
    }
  }
  CHECK_THAT(outline, WithinAbs(2 * 4 * 50, 1e-6));  // top and bottom squares
  CHECK_THAT(total(v, LineKind::Visible), WithinAbs(4 * 10, 1e-6));
}

TEST_CASE("Meshes show borders, creases and silhouettes, with hidden lines", "[draw][view]") {
  const Id a = Id::random();
  const Id b = Id::random();
  const auto near = box_mesh({0, 0, 0}, {10, 10, 10}, a);
  const auto far = box_mesh({5, 20, 5}, {15, 30, 15}, b);
  ViewProjector p(view_frame(vs::model::ViewDirection::Front), {}, kSettings);
  p.add_contents(a, near);
  p.add_contents(b, far);
  const auto v = p.run(true);
  // The near cube's square (its diagonals are flat, not creases), then the
  // far cube: visible outside the near one, hidden behind it.
  double av = 0, bv = 0, bh = 0;
  for (const auto& l : v.lines) {
    if (l.source == a && l.kind == LineKind::Visible) av += length(l);
    if (l.source == b && l.kind == LineKind::Visible) bv += length(l);
    if (l.source == b && l.kind == LineKind::Hidden) bh += length(l);
  }
  CHECK_THAT(av, WithinAbs(40, 1e-6));
  CHECK_THAT(bh, WithinAbs(10, 0.25));  // two 5 mm sides behind the near cube
  CHECK_THAT(bv, WithinAbs(30, 0.25));
}

TEST_CASE("Projection is the same on any thread count", "[draw][view]") {
  auto run = [](bool threaded) {
    ProjectionOptions o;
    if (threaded) {
      o.parallel = [](std::size_t n, const std::function<void(std::size_t)>& task) {
        // Out of order, as a pool might.
        for (std::size_t i = n; i-- > 0;) task(i);
      };
    }
    ViewProjector p(view_frame(vs::model::ViewDirection::Isometric), o, kSettings);
    auto c = vs::kernel::make_cylinder({{0, 0, 0}, {1, 0, 0}}, 8, 40, kSettings);
    REQUIRE(c);
    p.add_contents(Id::random(), *c);
    p.add_contents(Id::random(), box({-5, -20, -20}, {45, -12, 20}));
    return p.run(true);
  };
  const auto a = run(false);
  const auto b = run(true);
  REQUIRE(a.lines.size() == b.lines.size());
  for (std::size_t i = 0; i < a.lines.size(); ++i) {
    CHECK(a.lines[i].kind == b.lines[i].kind);
    CHECK(a.lines[i].points == b.lines[i].points);
  }
  CHECK(count(a, LineKind::Hidden) > 0);
}

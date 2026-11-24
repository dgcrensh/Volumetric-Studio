// Offsetting sketch curves (decision 0041): chains, exact line, arc and
// circle offsets, corners, Bézier fits within the minimum feature size, and
// what is refused.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vs/sketch/offset.hpp"
#include "vs/sketch/profile.hpp"

using namespace vs::sketch;
using Catch::Approx;
using vs::core::ProductSettings;

namespace {

const ProductSettings kSettings = ProductSettings::defaults();
constexpr double kPi = std::numbers::pi;

// A w × h rectangle from (0, 0), counter-clockwise; returns its four lines.
std::vector<Id> rectangle(Sketch& s, double w, double h) {
  const Id a = s.add_point({0, 0});
  const Id b = s.add_point({w, 0});
  const Id c = s.add_point({w, h});
  const Id d = s.add_point({0, h});
  return {s.add_line(a, b), s.add_line(b, c), s.add_line(c, d), s.add_line(d, a)};
}

std::size_t count_kind(const Sketch& s, const std::vector<Id>& ids, EntityKind kind) {
  return static_cast<std::size_t>(std::ranges::count_if(
      ids, [&](const Id& id) { return s.find_entity(id)->kind() == kind; }));
}

// The bounding box of the added curves' end points.
struct Box {
  double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
};
Box ends_box(const Sketch& s, const std::vector<Id>& curves) {
  Box b;
  for (const auto& id : curves) {
    std::vector<dvec2> pts;
    if (auto g = line_geometry(s, id)) pts = {g->start, g->end};
    if (auto g = arc_geometry(s, id)) pts = {g->start(), g->end()};
    for (const auto& p : pts) {
      b.x0 = std::min(b.x0, p.x);
      b.y0 = std::min(b.y0, p.y);
      b.x1 = std::max(b.x1, p.x);
      b.y1 = std::max(b.y1, p.y);
    }
  }
  return b;
}

// The distance from p to a Bézier, by dense sampling and refinement.
double distance_to(const BezierGeometry& g, dvec2 p) {
  constexpr int kN = 2000;
  double best_t = 0, best = std::numeric_limits<double>::infinity();
  for (int i = 0; i <= kN; ++i) {
    const double t = static_cast<double>(i) / kN;
    const double d = vs::core::distance(g.at(t), p);
    if (d < best) {
      best = d;
      best_t = t;
    }
  }
  // Golden-section refinement round the best sample.
  double lo = std::max(0.0, best_t - 1.0 / kN), hi = std::min(1.0, best_t + 1.0 / kN);
  for (int i = 0; i < 100; ++i) {
    const double m1 = lo + (hi - lo) * 0.381966, m2 = hi - (hi - lo) * 0.381966;
    if (vs::core::distance(g.at(m1), p) < vs::core::distance(g.at(m2), p)) hi = m2; else lo = m1;
  }
  return std::min(best, vs::core::distance(g.at(0.5 * (lo + hi)), p));
}

bool has_code(const vs::core::Diagnostic& d, std::string_view code) { return d.code == code; }

}  // namespace

TEST_CASE("Offset chains: order, direction and loops", "[offset]") {
  Sketch s;
  const auto rect = rectangle(s, 10, 6);
  // Given out of order: still one loop, from the earliest curve, along it.
  const std::vector<Id> given{rect[2], rect[0], rect[3], rect[1]};
  auto chains = offset_chains(s, given);
  REQUIRE(chains.ok());
  REQUIRE(chains->size() == 1);
  CHECK((*chains)[0].closed);
  CHECK((*chains)[0].curves == rect);
  CHECK((*chains)[0].reversed == std::vector<bool>(4, false));

  // An open chain starts at its loose end, curves reversed as needed.
  Sketch t;
  const Id p0 = t.add_point({0, 0}), p1 = t.add_point({5, 0}), p2 = t.add_point({5, 5});
  const Id l1 = t.add_line(p1, p0);  // runs backwards
  const Id l2 = t.add_line(p1, p2);
  const std::vector<Id> both{l1, l2};
  auto open = offset_chains(t, both);
  REQUIRE(open.ok());
  REQUIRE(open->size() == 1);
  CHECK_FALSE((*open)[0].closed);
  CHECK((*open)[0].curves == std::vector<Id>{l1, l2});
  CHECK((*open)[0].reversed == std::vector<bool>{true, false});  // from p0, the loose end

  // A point where three chosen curves meet is refused.
  const Id l3 = t.add_line(p1, t.add_point({9, 0}));
  const std::vector<Id> three{l1, l2, l3};
  auto branching = offset_chains(t, three);
  REQUIRE_FALSE(branching.ok());
  CHECK(branching.error().code == "sketch.offset.branching");

  // Points and empty selections aren't curves.
  const std::vector<Id> point{p0};
  CHECK(offset_chains(t, point).error().code == "sketch.offset.not_curve");
  CHECK(offset_chains(t, {}).error().code == "sketch.offset.empty");
}

TEST_CASE("Connected curves grow a selection to its chain", "[offset]") {
  Sketch s;
  const auto rect = rectangle(s, 10, 6);
  const std::vector<Id> one{rect[1]};
  CHECK(connected_curves(s, one) == rect);

  // A branch stops the growth at the branch point.
  const Id corner = s.find_entity(rect[1])->as<Line>()->end;
  const Id tail = s.add_line(corner, s.add_point({20, 20}));
  const std::vector<Id> grown = connected_curves(s, one);
  CHECK(std::ranges::find(grown, tail) == grown.end());
  CHECK(std::ranges::find(grown, rect[0]) != grown.end());  // through a two-curve corner
}

TEST_CASE("Offsetting a rectangle outward with sharp corners", "[offset]") {
  Sketch s;
  const auto rect = rectangle(s, 10, 6);
  const Sketch before = s;
  auto r = offset_curves(s, rect, {.distance = 2.0}, kSettings);
  REQUIRE(r.ok());
  CHECK(r->curves.size() == 4);
  CHECK(r->points.size() == 4);
  CHECK(count_kind(s, r->curves, EntityKind::Line) == 4);
  const Box b = ends_box(s, r->curves);
  CHECK(b.x0 == Approx(-2));
  CHECK(b.y0 == Approx(-2));
  CHECK(b.x1 == Approx(12));
  CHECK(b.y1 == Approx(8));
  // The source is untouched.
  for (const auto& e : before.entities()) CHECK(*s.find_entity(e.id) == e);
  CHECK(s.validate().empty());
  // Source and offset are two nested loops: a frame 2 mm wide.
  auto profiles = extract_profiles(s, kSettings);
  REQUIRE(profiles.ok());
  REQUIRE(profiles->size() == 1);
  CHECK((*profiles)[0].holes.size() == 1);
}

TEST_CASE("Offsetting a rectangle outward with round corners", "[offset]") {
  Sketch s;
  const auto rect = rectangle(s, 10, 6);
  auto r = offset_curves(s, rect, {.distance = 2.0, .corners = OffsetCorners::Round}, kSettings);
  REQUIRE(r.ok());
  CHECK(count_kind(s, r->curves, EntityKind::Line) == 4);
  CHECK(count_kind(s, r->curves, EntityKind::Arc) == 4);
  // Each corner arc is centred on the rectangle's corner point, radius 2.
  for (const auto& id : r->curves) {
    const Entity& e = *s.find_entity(id);
    if (const auto* a = e.as<Arc>()) {
      const bool on_corner = std::ranges::any_of(rect, [&](const Id& l) {
        return s.find_entity(l)->as<Line>()->start == a->center;
      });
      CHECK(on_corner);
      const auto g = arc_geometry(s, id);
      CHECK(g->radius == Approx(2));
      CHECK(g->end_angle - g->start_angle == Approx(kPi / 2));
    }
  }
  CHECK(s.validate().empty());
  auto profiles = extract_profiles(s, kSettings);
  REQUIRE(profiles.ok());
  CHECK((*profiles)[0].holes.size() == 1);
  // Area of the rounded outline: (10+4)(6+4) - (4 - π) 2².
  CHECK(std::abs((*profiles)[0].outer.area) == Approx(14 * 10 - (4 - kPi) * 4).epsilon(1e-6));
}

TEST_CASE("Insetting a rectangle trims its corners", "[offset]") {
  Sketch s;
  const auto rect = rectangle(s, 10, 6);
  for (const auto corners : {OffsetCorners::Extend, OffsetCorners::Round}) {
    Sketch t = s;
    auto r = offset_curves(t, rect, {.distance = -1.5, .corners = corners}, kSettings);
    REQUIRE(r.ok());
    CHECK(count_kind(t, r->curves, EntityKind::Line) == 4);
    CHECK(r->curves.size() == 4);
    const Box b = ends_box(t, r->curves);
    CHECK(b.x0 == Approx(1.5));
    CHECK(b.y0 == Approx(1.5));
    CHECK(b.x1 == Approx(8.5));
    CHECK(b.y1 == Approx(4.5));
  }

  // Deeper than half the height: refused, and the sketch is unchanged.
  const Sketch before = s;
  auto deep = offset_curves(s, rect, {.distance = -3.5}, kSettings);
  REQUIRE_FALSE(deep.ok());
  CHECK((deep.error().code == "sketch.offset.collapsed" ||
         deep.error().code == "sketch.offset.self_crossing"));
  CHECK(s == before);
}

TEST_CASE("Offsetting a slot keeps lines and arcs exact and joined", "[offset]") {
  // Two lines tangent to two half circles: every joint is smooth.
  Sketch s;
  const Id c1 = s.add_point({0, 0}), c2 = s.add_point({20, 0});
  const Id a = s.add_point({0, -5}), b = s.add_point({20, -5});
  const Id c = s.add_point({20, 5}), d = s.add_point({0, 5});
  const Id bottom = s.add_line(a, b);
  const Id right = s.add_arc(c2, b, c);
  const Id top = s.add_line(c, d);
  const Id left = s.add_arc(c1, d, a);
  const std::vector<Id> slot{bottom, right, top, left};
  for (const double d_mm : {3.0, -3.0}) {
    Sketch t = s;
    auto r = offset_curves(t, slot, {.distance = d_mm}, kSettings);
    REQUIRE(r.ok());
    CHECK(r->curves.size() == 4);  // no corner arcs at smooth joints
    for (const auto& id : r->curves) {
      if (const auto* arc = t.find_entity(id)->as<Arc>()) {
        CHECK((arc->center == c1 || arc->center == c2));  // concentric, shared centre
        CHECK(arc_geometry(t, id)->radius == Approx(5 + d_mm));
      }
    }
    CHECK(t.validate().empty());
  }
  // Insetting by the radius collapses the arcs.
  auto collapsed = offset_curves(s, slot, {.distance = -5.0}, kSettings);
  REQUIRE_FALSE(collapsed.ok());
  CHECK(has_code(collapsed.error(), "sketch.offset.collapsed"));
}

TEST_CASE("Offsetting a circle shares its centre", "[offset]") {
  Sketch s;
  const Id centre = s.add_point({3, 4});
  const Id circle = s.add_circle(centre, 5.0);
  const std::vector<Id> one{circle};
  auto r = offset_curves(s, one, {.distance = 2.0}, kSettings);
  REQUIRE(r.ok());
  REQUIRE(r->curves.size() == 1);
  const auto* c = s.find_entity(r->curves[0])->as<Circle>();
  REQUIRE(c != nullptr);
  CHECK(c->center == centre);
  CHECK(c->radius == Approx(7));
  auto inside = offset_curves(s, one, {.distance = -5.0}, kSettings);
  REQUIRE_FALSE(inside.ok());
  CHECK(inside.error().code == "sketch.offset.collapsed");
}

TEST_CASE("Offsetting an open L to each side", "[offset]") {
  Sketch s;
  const Id p0 = s.add_point({0, 0}), p1 = s.add_point({10, 0}), p2 = s.add_point({10, 10});
  const std::vector<Id> l{s.add_line(p0, p1), s.add_line(p1, p2)};
  // Left of travel is the inside of the turn: trimmed to (9, 1).
  {
    Sketch t = s;
    auto r = offset_curves(t, l, {.distance = 1.0}, kSettings);
    REQUIRE(r.ok());
    REQUIRE(r->curves.size() == 2);
    const auto g0 = line_geometry(t, r->curves[0]);
    const auto g1 = line_geometry(t, r->curves[1]);
    CHECK(g0->start.x == Approx(0));
    CHECK(g0->start.y == Approx(1));
    CHECK(g0->end.x == Approx(9));
    CHECK(g0->end.y == Approx(1));
    CHECK(g1->end.x == Approx(9));
    CHECK(g1->end.y == Approx(10));
  }
  // Right: the outside, extended to (11, -1), or rounded about (10, 0).
  {
    Sketch t = s;
    auto r = offset_curves(t, l, {.distance = -1.0}, kSettings);
    REQUIRE(r.ok());
    REQUIRE(r->curves.size() == 2);
    const auto g0 = line_geometry(t, r->curves[0]);
    CHECK(g0->end.x == Approx(11));
    CHECK(g0->end.y == Approx(-1));
  }
  {
    Sketch t = s;
    auto r = offset_curves(t, l, {.distance = -1.0, .corners = OffsetCorners::Round}, kSettings);
    REQUIRE(r.ok());
    REQUIRE(r->curves.size() == 3);
    const auto* arc = t.find_entity(r->curves[1])->as<Arc>();
    REQUIRE(arc != nullptr);
    CHECK(arc->center == p1);
  }
}

TEST_CASE("Sharp corners between a line and an arc extend to meet", "[offset]") {
  // A line up the y axis to (0, 10), then clockwise round (10, 0) to 60
  // degrees: a turn to the right that isn't smooth.
  Sketch s;
  const Id p0 = s.add_point({0, 0}), p1 = s.add_point({0, 10});
  const Id centre = s.add_point({10, 0});
  const double r = std::sqrt(200.0);
  const Id p2 = s.add_point({10 + r * std::cos(kPi / 3), r * std::sin(kPi / 3)});
  const Id line = s.add_line(p0, p1);
  const Id arc = s.add_arc(centre, p2, p1);  // counter-clockwise p2 → p1; the chain runs p1 → p2
  const std::vector<Id> chain{line, arc};
  for (const double d : {1.0, -1.0}) {
    Sketch t = s;
    auto res = offset_curves(t, chain, {.distance = d}, kSettings);
    REQUIRE(res.ok());
    CHECK(res->curves.size() == 2);  // extended or trimmed, never an added arc
    const auto lg = line_geometry(t, res->curves[0]);
    const auto ag = arc_geometry(t, res->curves[1]);
    REQUIRE(lg);
    REQUIRE(ag);
    // The corner lies on both: on the line's offset and the arc's.
    CHECK(vs::core::distance(lg->end, ag->center) == Approx(ag->radius).margin(1e-9));
    CHECK(ag->radius == Approx(r + d).margin(1e-9));  // clockwise along the chain: left is out
    CHECK(lg->end.x == Approx(-d).margin(1e-9));
  }
}

TEST_CASE("Bezier offsets stay within the minimum feature size", "[offset]") {
  Sketch s;
  const Id bz = s.add_bezier(dvec2{0, 0}, dvec2{10, 30}, dvec2{40, -20}, dvec2{50, 10});
  const BezierGeometry src = *bezier_geometry(s, bz);
  const std::vector<Id> one{bz};
  for (const double d : {2.0, -2.0}) {
    Sketch t = s;
    auto r = offset_curves(t, one, {.distance = d}, kSettings);
    REQUIRE(r.ok());
    CHECK(r->curves.size() > 1);
    CHECK(r->curves.size() < 200);
    double worst = 0;
    dvec2 prev_end{};
    for (std::size_t k = 0; k < r->curves.size(); ++k) {
      const BezierGeometry f = *bezier_geometry(t, r->curves[k]);
      if (k > 0) CHECK(f.start() == prev_end);  // joined
      prev_end = f.end();
      for (int i = 0; i <= 20; ++i) {
        const dvec2 q = f.at(i / 20.0);
        worst = std::max(worst, std::abs(distance_to(src, q) - std::abs(d)));
      }
      // Smooth from one fit to the next.
      if (k + 1 < r->curves.size()) {
        const BezierGeometry g = *bezier_geometry(t, r->curves[k + 1]);
        CHECK(std::abs(vs::core::cross(f.tangent(true), g.tangent(false))) < 1e-6);
      }
    }
    CHECK(worst <= kSettings.min_feature_size_mm());
    CHECK(t.validate().empty());
  }
}

TEST_CASE("A closed loop of Beziers offsets to a closed loop", "[offset]") {
  // A circle of radius 10 as four Béziers.
  constexpr double k = 0.5522847498 * 10;
  Sketch s;
  const Id e = s.add_point({10, 0}), n = s.add_point({0, 10});
  const Id w = s.add_point({-10, 0}), so = s.add_point({0, -10});
  const std::vector<Id> loop{
      s.add_bezier(e, s.add_point({10, k}), s.add_point({k, 10}), n),
      s.add_bezier(n, s.add_point({-k, 10}), s.add_point({-10, k}), w),
      s.add_bezier(w, s.add_point({-10, -k}), s.add_point({-k, -10}), so),
      s.add_bezier(so, s.add_point({k, -10}), s.add_point({10, -k}), e)};
  for (const double d : {3.0, -3.0}) {
    Sketch t = s;
    auto r = offset_curves(t, loop, {.distance = d}, kSettings);
    REQUIRE(r.ok());
    // Smooth joints: Béziers only, end to end, closed.
    CHECK(count_kind(t, r->curves, EntityKind::Bezier) == r->curves.size());
    const auto first = *bezier_geometry(t, r->curves.front());
    const auto last = *bezier_geometry(t, r->curves.back());
    CHECK(first.start() == last.end());
    // Radius close to 10 + d (the source is itself within 0.03 % of a circle).
    for (const auto& id : r->curves) {
      const auto g = *bezier_geometry(t, id);
      CHECK(vs::core::length(g.at(0.5)) == Approx(10 + d).epsilon(1e-3));
    }
    Sketch only_offset = t;
    for (const auto& id : loop) only_offset.remove_entity(id);
    auto profiles = extract_profiles(only_offset, kSettings);
    REQUIRE(profiles.ok());
    CHECK(profiles->size() == 1);
  }
  // Inset past the curvature folds it.
  auto folds = offset_curves(s, loop, {.distance = -10.5}, kSettings);
  REQUIRE_FALSE(folds.ok());
  CHECK(folds.error().code == "sketch.offset.collapsed");
}

TEST_CASE("Corners beside a Bezier are rounded", "[offset]") {
  // A line then a Bézier leaving at a right angle.
  Sketch s;
  const Id p0 = s.add_point({0, 0}), p1 = s.add_point({10, 0});
  const Id line = s.add_line(p0, p1);
  const Id bz = s.add_bezier(p1, s.add_point({10, 5}), s.add_point({15, 10}), s.add_point({20, 10}));
  const std::vector<Id> chain{line, bz};
  Sketch t = s;
  // Right of travel is the outside of the left turn.
  auto r = offset_curves(t, chain, {.distance = -1.0}, kSettings);
  REQUIRE(r.ok());
  REQUIRE(count_kind(t, r->curves, EntityKind::Arc) == 1);
  const auto* arc = t.find_entity(r->curves[1])->as<Arc>();
  REQUIRE(arc != nullptr);
  CHECK(arc->center == p1);
  // The inside is trimmed: no arc, the line shortened.
  Sketch u = s;
  auto in = offset_curves(u, chain, {.distance = 1.0}, kSettings);
  REQUIRE(in.ok());
  CHECK(count_kind(u, in->curves, EntityKind::Arc) == 0);
  const auto lg = line_geometry(u, in->curves[0]);
  CHECK(lg->end.x < 9.5);
  CHECK(lg->end.y == Approx(1));
  const auto bg = bezier_geometry(u, in->curves[1]);
  CHECK(vs::core::distance(bg->start(), lg->end) < 1e-9);
}

TEST_CASE("Offset distances under the minimum feature size are refused", "[offset]") {
  Sketch s;
  const auto rect = rectangle(s, 10, 6);
  CHECK(offset_curves(s, rect, {.distance = 0.0}, kSettings).error().code ==
        "sketch.offset.distance");
  CHECK(offset_curves(s, rect, {.distance = std::nan("")}, kSettings).error().code ==
        "sketch.offset.distance");
}

TEST_CASE("Offsets of polygons nest inside or round their source", "[offset]") {
  // Regular and star-shaped polygons, both corner kinds, both sides: the
  // source and its offset always make one profile with one hole.
  for (int sides = 3; sides <= 8; ++sides) {
    for (const bool star : {false, true}) {
      if (star && sides < 5) continue;
      Sketch s;
      std::vector<Id> pts;
      const int n = star ? 2 * sides : sides;
      for (int i = 0; i < n; ++i) {
        const double a = 2 * kPi * i / n + 0.1;
        const double r = star && i % 2 == 1 ? 12.0 : 20.0;
        pts.push_back(s.add_point({r * std::cos(a), r * std::sin(a)}));
      }
      std::vector<Id> lines;
      for (std::size_t i = 0; i < pts.size(); ++i) lines.push_back(s.add_line(pts[i], pts[(i + 1) % pts.size()]));
      for (const auto corners : {OffsetCorners::Extend, OffsetCorners::Round}) {
        for (const double d : {1.5, -1.0}) {
          INFO("sides " << sides << " star " << star << " round " << (corners == OffsetCorners::Round)
                        << " d " << d);
          Sketch t = s;
          auto r = offset_curves(t, lines, {.distance = d, .corners = corners}, kSettings);
          REQUIRE(r.ok());
          CHECK(t.validate().empty());
          auto profiles = extract_profiles(t, kSettings);
          REQUIRE(profiles.ok());
          REQUIRE(profiles->size() == 1);
          CHECK((*profiles)[0].holes.size() == 1);
        }
      }
    }
  }
}

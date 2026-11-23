// Cubic Bézier entities (decision 0040): geometry, validation, files,
// constraints at shared ends, profiles and faces.

#include <algorithm>
#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/check.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/sketch/profile.hpp"
#include "vs/sketch/serialize.hpp"
#include "vs/sketch/solver.hpp"

using namespace vs::sketch;
using Catch::Approx;
using vs::core::ProductSettings;

namespace {

const ProductSettings kSettings = ProductSettings::defaults();
constexpr double kPi = std::numbers::pi;

bool has_code(const vs::core::Diagnostics& ds, std::string_view code) {
  return std::ranges::any_of(ds, [&](const auto& d) { return d.code == code; });
}

// The polyline area of the curve, by many small chords.
double sampled_area(const BezierGeometry& g, int n = 20000) {
  double a = 0;
  dvec2 prev = g.at(0);
  for (int i = 1; i <= n; ++i) {
    const dvec2 next = g.at(static_cast<double>(i) / n);
    a += 0.5 * vs::core::cross(prev, next);
    prev = next;
  }
  return a;
}

// The largest distance from the curve's chords (in `n` equal steps) to the
// curve, measured at each step's middle against a much finer sampling.
double chord_deviation(const BezierGeometry& g, int n) {
  double worst = 0;
  for (int i = 0; i < n; ++i) {
    const double t0 = static_cast<double>(i) / n;
    const double t1 = static_cast<double>(i + 1) / n;
    const dvec2 a = g.at(t0);
    const dvec2 b = g.at(t1);
    for (int k = 1; k < 16; ++k) {
      const dvec2 p = g.at(t0 + (t1 - t0) * k / 16.0);
      const dvec2 ab = b - a;
      const double len = vs::core::length(ab);
      const double d = len > 0 ? std::abs(vs::core::cross(ab, p - a)) / len
                               : vs::core::length(p - a);
      worst = std::max(worst, d);
    }
  }
  return worst;
}

// A "D": a line from (0, 0) up to (0, 40) and a Bézier bulging right back
// down to (0, 0). Ids of the line, the curve and its points.
struct Dee {
  Sketch sketch;
  Id bottom, top, c1, c2, line, curve;
};

Dee dee(double bulge = 40.0) {
  Dee d;
  d.bottom = d.sketch.add_point({0, 0});
  d.top = d.sketch.add_point({0, 40});
  d.c1 = d.sketch.add_point({bulge, 40});
  d.c2 = d.sketch.add_point({bulge, 0});
  d.line = d.sketch.add_line(d.bottom, d.top);
  d.curve = d.sketch.add_bezier(d.top, d.c1, d.c2, d.bottom);
  return d;
}

// Distance from point p to the infinite line through a and b.
double off_line(dvec2 p, dvec2 a, dvec2 b) {
  return std::abs(vs::core::cross(b - a, p - a)) / vs::core::length(b - a);
}

}  // namespace

// --- geometry ---------------------------------------------------------------

TEST_CASE("A Bezier's geometry matches its definition", "[sketch][bezier]") {
  const BezierGeometry g{{{0, 0}, {10, 30}, {40, -10}, {50, 20}}};
  CHECK(g.at(0) == dvec2{0, 0});
  CHECK(g.at(1) == dvec2{50, 20});
  // B(1/2) = (p0 + 3 p1 + 3 p2 + p3) / 8.
  CHECK(g.at(0.5).x == Approx((0 + 30 + 120 + 50) / 8.0));
  CHECK(g.at(0.5).y == Approx((0 + 90 - 30 + 20) / 8.0));
  // B'(0) = 3 (p1 - p0), B'(1) = 3 (p3 - p2).
  CHECK(g.derivative(0).x == Approx(30));
  CHECK(g.derivative(0).y == Approx(90));
  CHECK(g.derivative(1).x == Approx(30));
  CHECK(g.derivative(1).y == Approx(90));
  // The derivative agrees with a difference quotient.
  const double h = 1e-6;
  const dvec2 diff = (g.at(0.3 + h) - g.at(0.3 - h)) / (2 * h);
  CHECK(g.derivative(0.3).x == Approx(diff.x).epsilon(1e-6));
  CHECK(g.derivative(0.3).y == Approx(diff.y).epsilon(1e-6));
}

TEST_CASE("A Bezier's area term is exact", "[sketch][bezier]") {
  for (const BezierGeometry& g :
       {BezierGeometry{{{0, 0}, {10, 30}, {40, -10}, {50, 20}}},
        BezierGeometry{{{-5, 3}, {7, 7}, {-2, 9}, {4, -6}}},
        BezierGeometry{{{1, 1}, {1, 1}, {1, 1}, {9, 1}}}}) {
    CHECK(g.area_term() == Approx(sampled_area(g)).epsilon(1e-6).margin(1e-9));
  }
}

TEST_CASE("A Bezier's tangent follows its handles", "[sketch][bezier]") {
  const BezierGeometry g{{{0, 0}, {0, 10}, {20, 10}, {20, 0}}};
  CHECK(g.tangent(false) == dvec2{0, 1});
  CHECK(g.tangent(true) == dvec2{0, -1});
  // A handle with no length: the next control point gives the direction.
  const BezierGeometry flat{{{0, 0}, {0, 0}, {10, 0}, {10, 5}}};
  CHECK(flat.tangent(false) == dvec2{1, 0});
  const BezierGeometry point{{{3, 3}, {3, 3}, {3, 3}, {3, 3}}};
  CHECK(point.tangent(false) == dvec2{0, 0});
}

TEST_CASE("Sampling a Bezier keeps every chord within the deflection", "[sketch][bezier]") {
  const BezierGeometry g{{{0, 0}, {300, 900}, {600, -900}, {900, 0}}};
  for (const double deflection : {1.0, 0.1, 1e-3}) {
    const int n = g.steps_within(deflection);
    CHECK(chord_deviation(g, n) <= deflection);
  }
  // A straight curve needs no more than the minimum.
  const BezierGeometry straight{{{0, 0}, {1, 0}, {2, 0}, {3, 0}}};
  CHECK(straight.steps_within(1e-6, 4) == 4);
}

// --- validation and files ----------------------------------------------------

TEST_CASE("A Bezier is validated like other curves", "[sketch][bezier]") {
  SECTION("well formed") {
    auto d = dee();
    CHECK(d.sketch.validate().empty());
    CHECK(d.sketch.find_entity(d.curve)->kind() == EntityKind::Bezier);
    CHECK(to_string(EntityKind::Bezier) == "bezier");
    CHECK(d.sketch.label(d.curve) == "Curve 1");
  }
  SECTION("a missing control point") {
    auto d = dee();
    d.sketch.remove_entity(d.c2);
    CHECK(has_code(d.sketch.validate(), "sketch.reference.missing"));
  }
  SECTION("a control point that isn't a point") {
    auto d = dee();
    auto* b = d.sketch.find_entity(d.curve)->as<Bezier>();
    b->control1 = d.line;
    CHECK(has_code(d.sketch.validate(), "sketch.reference.wrong_kind"));
  }
  SECTION("one point in two roles") {
    auto d = dee();
    auto* b = d.sketch.find_entity(d.curve)->as<Bezier>();
    b->control2 = b->control1;
    CHECK(has_code(d.sketch.validate(), "sketch.entity.degenerate"));
  }
}

TEST_CASE("A sketch with Beziers round-trips through JSON", "[sketch][bezier]") {
  auto d = dee();
  d.sketch.find_entity(d.curve)->name = "Bulge";
  d.sketch.add_bezier(dvec2{60, 0}, dvec2{70, 10}, dvec2{80, -10}, dvec2{90, 0}, /*construction=*/true);
  d.sketch.tangent(d.curve, d.line);
  const std::string text = to_json(d.sketch);
  CHECK(text.find("\"bezier\"") != std::string::npos);
  CHECK(text.find("\"control1\"") != std::string::npos);
  auto back = sketch_from_json(text);
  REQUIRE(back);
  CHECK(*back == d.sketch);
  CHECK(to_json(*back) == text);
}

// --- constraints -------------------------------------------------------------

TEST_CASE("Constraints that don't apply to a Bezier are refused", "[sketch][bezier]") {
  auto d = dee();
  const Id other = d.sketch.add_bezier(dvec2{100, 0}, dvec2{110, 10}, dvec2{120, 10}, dvec2{130, 0});
  const Id circle = d.sketch.add_circle(dvec2{200, 0}, 5);
  SECTION("point on a Bezier (until the solver knows curves)") {
    d.sketch.point_on(d.sketch.add_point({5, 5}), d.curve);
  }
  SECTION("equal") { d.sketch.equal(d.curve, other); }
  SECTION("radius") { d.sketch.radius(d.curve, 3); }
  SECTION("length") { d.sketch.length(d.curve, 3); }
  SECTION("tangent without a shared end") { d.sketch.tangent(d.curve, other); }
  SECTION("tangent to a circle") { d.sketch.tangent(d.curve, circle); }
  SECTION("horizontal") { d.sketch.horizontal(d.curve); }
  CHECK(has_code(d.sketch.validate(), "sketch.constraint.bad_form"));
}

TEST_CASE("A Bezier stays tangent to a line at their shared end", "[sketch][bezier]") {
  // A line from (-30, 0) to (0, 0), then a Bézier from (0, 0) whose first
  // handle points up and to the right.
  Sketch s;
  const Id a = s.add_point({-30, 0});
  const Id joint = s.add_point({0, 0});
  const Id h1 = s.add_point({10, 6});
  const Id h2 = s.add_point({30, 20});
  const Id e = s.add_point({40, 20});
  const Id line = s.add_line(a, joint);
  const Id curve = s.add_bezier(joint, h1, h2, e);
  s.fix(line);
  s.fix(h2);
  s.fix(e);
  s.tangent(curve, line);
  auto report = solve(s, kSettings);
  INFO(report.diagnostics.size());
  REQUIRE(report.ok());
  CHECK(off_line(*s.point(h1), *s.point(a), *s.point(joint)) < 1e-6);

  // Dragging the handle keeps it on the line's line.
  report = drag(s, h1, {15, 8}, kSettings);
  REQUIRE(report.ok());
  CHECK(off_line(*s.point(h1), *s.point(a), *s.point(joint)) < 1e-6);
  CHECK(s.point(h1)->x == Approx(15).margin(1e-6));
}

TEST_CASE("A Bezier stays tangent to an arc at their shared end", "[sketch][bezier]") {
  Sketch s;
  const Id arc = s.add_arc(dvec2{0, 0}, 20, 0, kPi / 2);  // ends at (0, 20)
  const auto* g = s.find_entity(arc)->as<Arc>();
  const Id joint = g->end;
  const Id centre = g->center;
  const Id h1 = s.add_point({-5, 28});
  const Id h2 = s.add_point({-20, 30});
  const Id e = s.add_point({-30, 30});
  const Id curve = s.add_bezier(joint, h1, h2, e);
  s.fix(arc);
  s.fix(h2);
  s.fix(e);
  s.tangent(arc, curve);
  const auto report = solve(s, kSettings);
  REQUIRE(report.ok());
  // The handle runs square to the radius at the joint: along -x here.
  const dvec2 radius = *s.point(joint) - *s.point(centre);
  const dvec2 handle = *s.point(h1) - *s.point(joint);
  CHECK(std::abs(vs::core::dot(radius, handle)) < 1e-6 * vs::core::length(radius));
}

TEST_CASE("Two Beziers stay smooth at their shared end", "[sketch][bezier]") {
  Sketch s;
  const Id p0 = s.add_point({0, 0});
  const Id a1 = s.add_point({10, 10});
  const Id a2 = s.add_point({20, 10});
  const Id joint = s.add_point({30, 0});
  const Id b1 = s.add_point({35, -12});
  const Id b2 = s.add_point({50, -10});
  const Id p3 = s.add_point({60, 0});
  const Id first = s.add_bezier(p0, a1, a2, joint);
  const Id second = s.add_bezier(joint, b1, b2, p3);
  for (const Id& id : {p0, a1, joint, b2, p3}) s.fix(id);
  s.tangent(first, second);
  const auto report = solve(s, kSettings);
  REQUIRE(report.ok());
  CHECK(off_line(*s.point(joint), *s.point(a2), *s.point(b1)) < 1e-6);
}

TEST_CASE("Fixing a Bezier fixes its four points", "[sketch][bezier]") {
  auto d = dee();
  d.sketch.fix(d.curve);  // the line's ends are the curve's ends
  const auto report = solve(d.sketch, kSettings);
  CHECK(report.status == SolveStatus::Solved);
  CHECK(report.dof == 0);
  drag(d.sketch, d.c1, {60, 60}, kSettings);
  CHECK(d.sketch.point(d.c1)->x == Approx(40).margin(1e-6));
  CHECK(d.sketch.point(d.c1)->y == Approx(40).margin(1e-6));
}

TEST_CASE("An unconstrained Bezier reports its points as free", "[sketch][bezier]") {
  Sketch s;
  const Id curve = s.add_bezier(dvec2{0, 0}, dvec2{10, 10}, dvec2{20, 10}, dvec2{30, 0});
  const auto report = solve(s, kSettings);
  CHECK(report.status == SolveStatus::UnderConstrained);
  CHECK(report.dof == 8);
  CHECK(std::ranges::find(report.free_entities, curve) == report.free_entities.end());
  CHECK(report.free_entities.size() == 4);
}

// --- profiles and faces ------------------------------------------------------

TEST_CASE("A line and a Bezier close a profile with the exact area", "[sketch][bezier]") {
  auto d = dee();
  auto profiles = extract_profiles(d.sketch, kSettings);
  REQUIRE(profiles);
  REQUIRE(profiles->size() == 1);
  const auto bz = bezier_geometry(d.sketch, d.curve);
  REQUIRE(bz);
  // The line contributes nothing (it lies on x = 0 through the origin).
  CHECK(profiles->front().area() == Approx(std::abs(bz->area_term())));
  CHECK(profiles->front().area() == Approx(960));  // 3/5 of the control square

  auto face = make_face(d.sketch, profiles->front(), kSettings);
  REQUIRE(face);
  CHECK(vs::kernel::check_validity(*face).empty());
  CHECK(vs::kernel::surface_area(*face) == Approx(profiles->front().area()).epsilon(1e-9));

  auto solid = vs::kernel::make_prism(*face, {0, 0, 10}, kSettings);
  REQUIRE(solid);
  CHECK(vs::kernel::check_validity(*solid).empty());
  CHECK(vs::kernel::classify(*solid, {15, 20, 5}, kSettings) == vs::kernel::PointClass::Inside);
  CHECK(vs::kernel::classify(*solid, {35, 2, 5}, kSettings) == vs::kernel::PointClass::Outside);
}

TEST_CASE("A closed chain of Beziers bounds a hole", "[sketch][bezier]") {
  Sketch s;
  // Outer: a 100 x 100 square. Inner: a blob of four Béziers round (50, 50).
  const Id q0 = s.add_point({0, 0});
  const Id q1 = s.add_point({100, 0});
  const Id q2 = s.add_point({100, 100});
  const Id q3 = s.add_point({0, 100});
  s.add_line(q0, q1);
  s.add_line(q1, q2);
  s.add_line(q2, q3);
  s.add_line(q3, q0);
  const double k = 0.5522847498 * 20;  // circle-like handles, radius 20
  const Id e = s.add_point({70, 50});
  const Id n = s.add_point({50, 70});
  const Id w = s.add_point({30, 50});
  const Id so = s.add_point({50, 30});
  auto handle = [&](dvec2 p) { return s.add_point(p); };
  s.add_bezier(e, handle({70, 50 + k}), handle({50 + k, 70}), n);
  s.add_bezier(n, handle({50 - k, 70}), handle({30, 50 + k}), w);
  s.add_bezier(w, handle({30, 50 - k}), handle({50 - k, 30}), so);
  s.add_bezier(so, handle({50 + k, 30}), handle({70, 50 - k}), e);
  auto profiles = extract_profiles(s, kSettings);
  REQUIRE(profiles);
  REQUIRE(profiles->size() == 1);
  REQUIRE(profiles->front().holes.size() == 1);
  CHECK(profiles->front().holes.front().edges.size() == 4);
  // Close to a circle of radius 20 (the approximation is within 0.03 %).
  CHECK(profiles->front().area() == Approx(10000 - kPi * 400).epsilon(1e-3));
  auto faces = make_faces(s, kSettings);
  REQUIRE(faces);
  CHECK(vs::kernel::check_validity(faces->front()).empty());
  CHECK(vs::kernel::surface_area(faces->front()) == Approx(profiles->front().area()).epsilon(1e-9));
}

TEST_CASE("A Bezier that loops over itself is a self-crossing profile", "[sketch][bezier]") {
  Sketch s;
  const Id a = s.add_point({0, 0});
  const Id b = s.add_point({20, 0});
  s.add_line(a, b);
  // From (20, 0) the handles cross over, making a loop, then back to (0, 0).
  s.add_bezier(b, s.add_point({-30, 40}), s.add_point({50, 40}), a);
  const auto profiles = extract_profiles(s, kSettings);
  REQUIRE_FALSE(profiles);
  CHECK(profiles.error().code == "sketch.profile.self_crossing");
}

TEST_CASE("A Bezier left open is an open profile", "[sketch][bezier]") {
  Sketch s;
  s.add_bezier(dvec2{0, 0}, dvec2{10, 10}, dvec2{20, 10}, dvec2{30, 0});
  const auto profiles = extract_profiles(s, kSettings);
  REQUIRE_FALSE(profiles);
  CHECK(profiles.error().code == "sketch.profile.open");
}

TEST_CASE("A reversed Bezier in a loop still makes a valid face", "[sketch][bezier]") {
  // The same D drawn clockwise: the curve runs from bottom to top.
  Sketch s;
  const Id bottom = s.add_point({0, 0});
  const Id top = s.add_point({0, 40});
  s.add_line(top, bottom);
  s.add_bezier(bottom, s.add_point({40, 0}), s.add_point({40, 40}), top);
  auto faces = make_faces(s, kSettings);
  REQUIRE(faces);
  CHECK(vs::kernel::check_validity(faces->front()).empty());
  CHECK(vs::kernel::surface_area(faces->front()) == Approx(960));
}

#include <algorithm>
#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "canonical.hpp"
#include "vs/sketch/solver.hpp"

using namespace vs::sketch;
using vs::core::ProductSettings;

namespace {

const ProductSettings kSettings = ProductSettings::defaults();
constexpr double kPi = std::numbers::pi;
// Solved positions must be this close to the analytic answer (mm).
constexpr double kEps = 1e-6;

void check_point(const Sketch& s, const Id& p, dvec2 expected) {
  const auto at = s.point(p);
  REQUIRE(at);
  CHECK(at->x == Catch::Approx(expected.x).margin(kEps));
  CHECK(at->y == Catch::Approx(expected.y).margin(kEps));
}

bool contains(const std::vector<Id>& ids, const Id& id) {
  return std::ranges::find(ids, id) != ids.end();
}

}  // namespace

// --- canonical sketches ---------------------------------------------------

TEST_CASE("A rectangle solves to its dimensions", "[sketch][solver]") {
  auto r = test::rectangle(40, 25);
  const auto report = solve(r.sketch, kSettings);
  INFO(report.diagnostics.size());
  CHECK(report.status == SolveStatus::Solved);
  CHECK(report.ok());
  CHECK(report.applied);
  CHECK(report.dof == 0);
  CHECK(report.free_entities.empty());
  CHECK(report.conflicting.empty());
  CHECK(report.redundant.empty());
  CHECK_FALSE(report.diagnostics.has_errors());
  check_point(r.sketch, r.p00, {0, 0});
  check_point(r.sketch, r.p10, {40, 0});
  check_point(r.sketch, r.p11, {40, 25});
  check_point(r.sketch, r.p01, {0, 25});
}

TEST_CASE("A slot solves to tangent lines between its end arcs", "[sketch][solver]") {
  auto r = test::slot(50, 8);
  const auto report = solve(r.sketch, kSettings);
  CHECK(report.status == SolveStatus::Solved);
  CHECK(report.dof == 0);
  check_point(r.sketch, r.c1, {0, 0});
  check_point(r.sketch, r.c2, {50, 0});
  check_point(r.sketch, r.top_left, {0, 8});
  check_point(r.sketch, r.top_right, {50, 8});
  check_point(r.sketch, r.bottom_left, {0, -8});
  check_point(r.sketch, r.bottom_right, {50, -8});
  const auto left = arc_geometry(r.sketch, r.left_arc);
  REQUIRE(left);
  CHECK(left->radius == Catch::Approx(8).margin(kEps));
  // Counter-clockwise from the top to the bottom: the left half.
  CHECK(left->end_angle - left->start_angle == Catch::Approx(kPi).margin(1e-9));
}

TEST_CASE("A gear-like profile solves to its tooth points", "[sketch][solver]") {
  auto r = test::gear(12, 40, 46);
  const auto report = solve(r.sketch, kSettings);
  CHECK(report.status == SolveStatus::Solved);
  CHECK(report.dof == 0);
  REQUIRE(r.points.size() == 48);
  for (std::size_t i = 0; i < r.points.size(); ++i) {
    INFO("point " << i);
    check_point(r.sketch, r.points[i], r.expected[i]);
  }
}

TEST_CASE("A bolt circle solves to evenly spaced holes", "[sketch][solver]") {
  auto r = test::bolt_circle(50, 35, 6, 8);
  const auto report = solve(r.sketch, kSettings);
  CHECK(report.status == SolveStatus::Solved);
  CHECK(report.dof == 0);
  for (int k = 0; k < 6; ++k) {
    INFO("hole " << k);
    const double a = 2 * kPi * k / 6;
    check_point(r.sketch, r.hole_centers[static_cast<std::size_t>(k)],
                {35 * std::cos(a), 35 * std::sin(a)});
    const auto hole = circle_geometry(r.sketch, r.holes[static_cast<std::size_t>(k)]);
    REQUIRE(hole);
    CHECK(hole->radius == Catch::Approx(4).margin(kEps));
  }
  const auto plate = circle_geometry(r.sketch, r.plate);
  REQUIRE(plate);
  CHECK(plate->radius == Catch::Approx(50).margin(kEps));
}

TEST_CASE("Solving a solved sketch changes nothing", "[sketch][solver]") {
  auto r = test::rectangle(40, 25);
  REQUIRE(solve(r.sketch, kSettings).ok());
  const Sketch before = r.sketch;
  REQUIRE(solve(r.sketch, kSettings).ok());
  for (const auto& e : before.entities()) {
    if (const auto p = before.point(e.id)) check_point(r.sketch, e.id, *p);
  }
}

TEST_CASE("Each constraint kind solves to its definition", "[sketch][solver]") {
  Sketch s;
  const Id a = s.add_point({0, 0});
  s.fix(a);

  SECTION("horizontal and vertical distance") {
    const Id b = s.add_point({3, 4});
    s.add_constraint(ConstraintKind::HorizontalDistance, {a, b}, 12.5);
    s.add_constraint(ConstraintKind::VerticalDistance, {a, b}, -7.25);
    REQUIRE(solve(s, kSettings).status == SolveStatus::Solved);
    check_point(s, b, {12.5, -7.25});
  }
  SECTION("angle of a line, and a point-to-line distance") {
    const Id b = s.add_point({10, 1});
    const Id l = s.add_line(a, b);
    s.length(l, 20);
    s.add_constraint(ConstraintKind::Angle, {l}, kPi / 6);
    // p is 5 mm right of a and 3 mm from the line, on the side it starts.
    const Id p = s.add_point({5, 9});
    s.add_constraint(ConstraintKind::HorizontalDistance, {a, p}, 5.0);
    s.distance(p, l, 3);
    REQUIRE(solve(s, kSettings).status == SolveStatus::Solved);
    check_point(s, b, {20 * std::cos(kPi / 6), 20 * std::sin(kPi / 6)});
    check_point(s, p, {5, (3 + 5 * std::sin(kPi / 6)) / std::cos(kPi / 6)});
  }
  SECTION("perpendicular, parallel and equal lines") {
    const Id l1 = s.add_line(a, s.add_point({10, 0.5}));
    s.horizontal(l1);
    s.length(l1, 10);
    const Id b = s.add_point({1, 8});
    const Id l2 = s.add_line(a, b);
    s.perpendicular(l1, l2);
    s.equal(l1, l2);
    const Id c = s.add_point({3, 3});
    const Id d = s.add_point({4, 9});
    const Id l3 = s.add_line(c, d);
    s.fix(c);
    s.parallel(l2, l3);
    s.length(l3, 5);
    REQUIRE(solve(s, kSettings).status == SolveStatus::Solved);
    check_point(s, b, {0, 10});
    check_point(s, d, {3, 8});
  }
  SECTION("symmetric about a line and about a point") {
    const Id axis = s.add_line(a, s.add_point({0.2, 10}));
    s.vertical(axis);
    s.length(axis, 10);
    const Id p = s.add_point({-4, 3});
    const Id q = s.add_point({3.5, 2.5});
    s.fix(p);
    s.symmetric(p, q, axis);
    const Id m = s.add_point({1, 1});
    const Id n = s.add_point({-1, -2});
    s.fix(m);
    s.symmetric(m, n, a);
    REQUIRE(solve(s, kSettings).status == SolveStatus::Solved);
    check_point(s, q, {4, 3});
    check_point(s, n, {-1, -1});
  }
  SECTION("a point on a circle, and tangent circles") {
    const Id c1 = s.add_circle(a, 4);
    s.radius(c1, 5);
    const Id p = s.add_point({4, 4});
    s.point_on(p, c1);
    s.add_constraint(ConstraintKind::Horizontal, {a, p});
    const Id b = s.add_point({9, 0.3});
    const Id c2 = s.add_circle(b, 2);
    s.add_constraint(ConstraintKind::Horizontal, {a, b});
    s.diameter(c2, 6);
    s.tangent(c1, c2);
    REQUIRE(solve(s, kSettings).status == SolveStatus::Solved);
    check_point(s, p, {5, 0});
    check_point(s, b, {8, 0});
  }
}

// --- under-constrained ----------------------------------------------------

TEST_CASE("An under-constrained sketch reports its freedoms", "[sketch][solver]") {
  auto r = test::rectangle(40, 25, /*dimensioned_width=*/false);
  const auto report = solve(r.sketch, kSettings);
  CHECK(report.status == SolveStatus::UnderConstrained);
  CHECK(report.ok());
  CHECK(report.applied);
  CHECK(report.dof == 1);
  // The right-hand corners can slide; the fixed corner and its neighbour
  // on the left edge can't.
  CHECK(contains(report.free_entities, r.p10));
  CHECK(contains(report.free_entities, r.p11));
  CHECK_FALSE(contains(report.free_entities, r.p00));
  CHECK_FALSE(contains(report.free_entities, r.p01));

  const auto found = report.diagnostics.with_code("sketch.solve.under_constrained");
  REQUIRE(found.size() == 1);
  CHECK(found[0]->severity == vs::core::Severity::Info);
  CHECK(found[0]->subjects.back().kind == vs::core::SubjectKind::Sketch);
}

TEST_CASE("A sketch with no constraints is entirely free", "[sketch][solver]") {
  Sketch s;
  const Id l = s.add_line(dvec2{0, 0}, dvec2{1, 1});
  const auto report = solve(s, kSettings);
  CHECK(report.status == SolveStatus::UnderConstrained);
  CHECK(report.dof == 4);
  CHECK(report.free_entities.size() == 2);
  (void)l;
}

// --- over-constrained -----------------------------------------------------

TEST_CASE("Conflicting constraints are named and the sketch is left alone",
          "[sketch][solver]") {
  auto r = test::rectangle(40, 25);
  // A diagonal that can't be 10 mm in a 40 × 25 rectangle.
  const Id diagonal = r.sketch.distance(r.p00, r.p11, 10);
  r.sketch.find_constraint(diagonal)->name = "Diagonal";
  const Sketch before = r.sketch;

  const auto report = solve(r.sketch, kSettings);
  CHECK(report.status == SolveStatus::OverConstrained);
  CHECK_FALSE(report.ok());
  CHECK_FALSE(report.applied);
  CHECK(r.sketch == before);
  CHECK(contains(report.conflicting, diagonal));

  const auto found = report.diagnostics.with_code("sketch.solve.conflicting");
  REQUIRE(found.size() == 1);
  CHECK(found[0]->is_error());
  // Every conflicting constraint is a subject, by id and by name.
  bool names_diagonal = false;
  for (const auto& subject : found[0]->subjects) {
    if (subject.id == diagonal) names_diagonal = subject.label == "Diagonal";
  }
  CHECK(names_diagonal);
  CHECK(found[0]->target()->kind == vs::core::SubjectKind::Constraint);
  CHECK(found[0]->subjects.back().kind == vs::core::SubjectKind::Sketch);
}

TEST_CASE("Two lengths for one line conflict", "[sketch][solver]") {
  auto r = test::rectangle(40, 25);
  const Id second = r.sketch.length(r.bottom, 30);
  const auto report = solve(r.sketch, kSettings);
  CHECK(report.status == SolveStatus::OverConstrained);
  CHECK(contains(report.conflicting, r.width));
  CHECK(contains(report.conflicting, second));
}

TEST_CASE("Redundant constraints are named but the sketch still solves", "[sketch][solver]") {
  auto r = test::rectangle(40, 25);
  // Top and bottom are already both horizontal.
  const Id extra = r.sketch.parallel(r.top, r.bottom);
  const auto report = solve(r.sketch, kSettings);
  CHECK(report.status == SolveStatus::OverConstrained);
  CHECK(report.applied);
  CHECK(report.conflicting.empty());
  REQUIRE(report.redundant.size() == 1);
  // PlaneGCS picks one of the three that say the same thing.
  const Id named = report.redundant.front();
  CHECK((named == extra || named == r.h_top || named == r.h_bottom));
  const auto found = report.diagnostics.with_code("sketch.solve.redundant");
  REQUIRE(found.size() == 1);
  CHECK(found[0]->severity == vs::core::Severity::Warning);
  CHECK(found[0]->target()->id == named);
  check_point(r.sketch, r.p11, {40, 25});
}

// --- structural errors ----------------------------------------------------

TEST_CASE("A malformed sketch is reported, not solved", "[sketch][solver]") {
  Sketch s;
  const Id p = s.add_point({0, 0});
  const Id c = s.add_circle(p, 3);
  s.add_constraint(ConstraintKind::Horizontal, {c});  // a circle can't be horizontal
  const auto report = solve(s, kSettings);
  CHECK(report.status == SolveStatus::Failed);
  CHECK_FALSE(report.applied);
  CHECK(report.diagnostics.with_code("sketch.constraint.bad_form").size() == 1);
}

// --- dragging -------------------------------------------------------------

TEST_CASE("Dragging a free point moves it and the sketch follows", "[sketch][solver]") {
  auto r = test::rectangle(40, 25, /*dimensioned_width=*/false);
  REQUIRE(solve(r.sketch, kSettings).ok());

  const auto report = drag(r.sketch, r.p11, {60, 30}, kSettings);
  CHECK(report.ok());
  CHECK(report.applied);
  // The width follows the drag; the height is fixed at 25, so the corner
  // stops at y = 25 and the other right-hand corner comes along.
  check_point(r.sketch, r.p11, {60, 25});
  check_point(r.sketch, r.p10, {60, 0});
  check_point(r.sketch, r.p00, {0, 0});
  check_point(r.sketch, r.p01, {0, 25});
}

TEST_CASE("Dragging a fully constrained point leaves it in place", "[sketch][solver]") {
  auto r = test::rectangle(40, 25);
  REQUIRE(solve(r.sketch, kSettings).ok());
  const auto report = drag(r.sketch, r.p11, {60, 30}, kSettings);
  CHECK(report.ok());
  check_point(r.sketch, r.p11, {40, 25});
}

TEST_CASE("Only points can be dragged", "[sketch][solver]") {
  auto r = test::rectangle(40, 25);
  const auto report = drag(r.sketch, r.top, {1, 1}, kSettings);
  CHECK(report.status == SolveStatus::Failed);
  CHECK(report.diagnostics.with_code("sketch.solve.drag_not_point").size() == 1);
}

TEST_CASE("Solving through the interface matches the free functions", "[sketch][solver]") {
  auto a = test::slot(30, 5);
  auto b = a;
  const auto solver = make_solver();
  const auto ra = solver->solve(a.sketch, kSettings);
  const auto rb = solve(b.sketch, kSettings);
  CHECK(ra.status == rb.status);
  CHECK(test::same_geometry(a.sketch, b.sketch));
}

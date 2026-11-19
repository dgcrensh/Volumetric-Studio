#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vs/sketch/sketch.hpp"

using namespace vs::sketch;

TEST_CASE("Adding geometry creates points and keeps ids", "[sketch]") {
  Sketch s;
  const Id line = s.add_line(dvec2{0, 0}, dvec2{10, 0});
  REQUIRE(s.entities().size() == 3);  // two points and the line
  const Entity* e = s.find_entity(line);
  REQUIRE(e);
  CHECK(e->kind() == EntityKind::Line);
  const auto g = line_geometry(s, line);
  REQUIRE(g);
  CHECK(g->end.x == 10);

  const Id fixed = Id::random();
  CHECK(s.add_point({1, 2}, false, fixed) == fixed);
  CHECK(s.point(fixed) == dvec2{1, 2});
  CHECK_FALSE(s.point(line));  // a line is not a point
  CHECK(s.validate().empty());
}

TEST_CASE("Arcs run counter-clockwise from start to end", "[sketch]") {
  Sketch s;
  const double pi = std::numbers::pi;
  const Id arc = s.add_arc(dvec2{1, 1}, 2.0, pi / 2, pi);
  const auto g = arc_geometry(s, arc);
  REQUIRE(g);
  CHECK(g->radius == Catch::Approx(2));
  CHECK(g->end_angle - g->start_angle == Catch::Approx(pi / 2));
  // The other way round is the long way.
  const Id back = s.add_arc(dvec2{1, 1}, 2.0, pi, pi / 2);
  CHECK(arc_geometry(s, back)->end_angle - arc_geometry(s, back)->start_angle ==
        Catch::Approx(3 * pi / 2));
}

TEST_CASE("Labels prefer names and count by kind", "[sketch]") {
  Sketch s(Id::random(), "Base");
  const Id a = s.add_line(dvec2{0, 0}, dvec2{1, 0});
  const Id b = s.add_line(dvec2{0, 1}, dvec2{1, 1});
  const Id h = s.horizontal(a);
  CHECK(s.label(a) == "Line 1");
  CHECK(s.label(b) == "Line 2");
  CHECK(s.label(h) == "Horizontal 1");
  s.find_entity(b)->name = "Top edge";
  CHECK(s.label(b) == "Top edge");
  CHECK(s.label(s.id()) == "Base");
  CHECK(s.subject(h).kind == vs::core::SubjectKind::Constraint);
  CHECK(s.subject(a).kind == vs::core::SubjectKind::SketchEntity);
  CHECK(s.sketch_subject().kind == vs::core::SubjectKind::Sketch);
}

TEST_CASE("Constraint kinds round-trip through their names", "[sketch]") {
  for (int k = 0; k <= static_cast<int>(ConstraintKind::Fix); ++k) {
    const auto kind = static_cast<ConstraintKind>(k);
    CHECK(parse_constraint_kind(to_string(kind)) == kind);
  }
  CHECK_FALSE(parse_constraint_kind("glue"));
  CHECK(is_dimensional(ConstraintKind::Distance));
  CHECK_FALSE(is_dimensional(ConstraintKind::Tangent));
}

TEST_CASE("Validation names broken references and bad forms", "[sketch]") {
  Sketch s;
  const Id p = s.add_point({0, 0});
  const Id q = s.add_point({1, 0});
  const Id line = s.add_line(p, q);
  const Id circle = s.add_circle(p, 2);

  SECTION("a deleted point leaves its line and constraints broken") {
    const Id c = s.coincident(p, q);
    s.remove_entity(q);
    const auto d = s.validate();
    const auto missing = d.with_code("sketch.reference.missing");
    REQUIRE(missing.size() == 2);
    CHECK(missing[0]->target()->id == line);
    CHECK(missing[1]->target()->id == c);
  }
  SECTION("a constraint on the wrong geometry") {
    const Id c = s.parallel(line, circle);
    const auto d = s.validate();
    REQUIRE(d.with_code("sketch.constraint.bad_form").size() == 1);
    CHECK(d.with_code("sketch.constraint.bad_form")[0]->target()->id == c);
  }
  SECTION("the same entity twice") {
    s.coincident(p, p);
    CHECK(s.validate().with_code("sketch.constraint.bad_form").size() == 1);
  }
  SECTION("bad values") {
    s.radius(circle, -1);
    s.length(line, std::nan(""));
    CHECK(s.validate().with_code("sketch.constraint.bad_value").size() == 2);
  }
  SECTION("a line through one point") {
    s.add_line(p, p);
    CHECK(s.validate().with_code("sketch.entity.degenerate").size() == 1);
  }
  SECTION("a curve whose end is not a point") {
    s.add_line(p, line);
    CHECK(s.validate().with_code("sketch.reference.wrong_kind").size() == 1);
  }
  SECTION("a duplicated id") {
    s.add_point({5, 5}, false, p);
    CHECK(s.validate().with_code("sketch.entity.duplicate_id").size() == 1);
  }
}

TEST_CASE("Dimensions keep their expression", "[sketch]") {
  Sketch s;
  const Id line = s.add_line(dvec2{0, 0}, dvec2{1, 0});
  const Id d = s.length(line, 12);
  s.find_constraint(d)->dimension->expression = "width / 2";
  CHECK(s.set_dimension(d, 14));
  CHECK(s.find_constraint(d)->dimension->value == 14);
  CHECK(s.find_constraint(d)->dimension->expression == "width / 2");
  CHECK_FALSE(s.set_dimension(s.horizontal(line), 3));  // not dimensional
}

TEST_CASE("A sketch plane maps sketch points into model space", "[sketch]") {
  SketchPlane xz{{0, 5, 0}, {1, 0, 0}, {0, 0, 1}};
  CHECK(xz.to_model({2, 3}) == dvec3{2, 5, 3});
  CHECK(xz.normal() == dvec3{0, -1, 0});
}

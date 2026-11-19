// Sketch mode's editing model: drawing tools, constraints and dimensions,
// live solving with degrees of freedom, conflicts by name.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "sketch_editor.hpp"
#include "vs/sketch/profile.hpp"

using namespace vs;
using core::Id;
using app::SketchEditor;
using app::SketchTool;
using Catch::Matchers::WithinAbs;
using sketch::ConstraintKind;
using sketch::SolveStatus;

namespace {

const auto kSettings = core::ProductSettings::defaults();

std::size_t count(const sketch::Sketch& s, sketch::EntityKind kind) {
  return static_cast<std::size_t>(
      std::count_if(s.entities().begin(), s.entities().end(), [&](const auto& e) { return e.kind() == kind; }));
}

std::size_t count(const sketch::Sketch& s, ConstraintKind kind) {
  return static_cast<std::size_t>(
      std::count_if(s.constraints().begin(), s.constraints().end(), [&](const auto& c) { return c.kind == kind; }));
}

std::vector<Id> of_kind(const sketch::Sketch& s, sketch::EntityKind kind) {
  std::vector<Id> out;
  for (const auto& e : s.entities()) {
    if (e.kind() == kind) out.push_back(e.id);
  }
  return out;
}

// Closed profiles; 0 when the geometry is open (sketch.profile.open).
std::size_t profiles(const sketch::Sketch& s) {
  const auto p = sketch::extract_profiles(s, kSettings);
  if (!p) return 0;
  return p->size();
}

}  // namespace

TEST_CASE("The rectangle tool draws four lines kept square, and dimensions fix it", "[app][sketch]") {
  SketchEditor ed(sketch::Sketch(Id::random(), "Outline"), kSettings);
  CHECK(ed.dof() == 0);
  ed.set_tool(SketchTool::Rectangle);
  CHECK(ed.click({0, 0}).empty());
  CHECK(ed.pending().size() == 1);
  const auto added = ed.click({60, 40});
  CHECK(added.size() == 8);
  CHECK(ed.pending().empty());
  const auto& s = ed.sketch();
  CHECK(count(s, sketch::EntityKind::Line) == 4);
  CHECK(count(s, sketch::EntityKind::Point) == 4);
  CHECK(count(s, ConstraintKind::Horizontal) == 2);
  CHECK(count(s, ConstraintKind::Vertical) == 2);
  CHECK(ed.status() == SolveStatus::UnderConstrained);
  CHECK(ed.dof() == 4);
  CHECK(ed.status_text() == "Under-constrained: 4 degrees of freedom");
  CHECK(profiles(s) == 1);

  // Fix a corner and dimension two sides: fully constrained.
  const auto lines = of_kind(s, sketch::EntityKind::Line);
  const Id corner = ed.nearest_point({0.5, -0.5}, 2.0);
  REQUIRE_FALSE(corner.is_null());
  REQUIRE_FALSE(ed.constrain(ConstraintKind::Fix, {corner}).is_null());
  const Id width = ed.constrain(ConstraintKind::Distance, {lines[0]}, 50.0);
  REQUIRE_FALSE(width.is_null());
  const Id height = ed.constrain(ConstraintKind::Distance, {lines[1]});  // measured: 40
  REQUIRE_FALSE(height.is_null());
  CHECK_THAT(ed.sketch().find_constraint(height)->dimension->value, WithinAbs(40.0, 1e-9));
  CHECK(ed.status() == SolveStatus::Solved);
  CHECK(ed.dof() == 0);
  CHECK(ed.status_text() == "Fully constrained");
  const auto g = sketch::line_geometry(ed.sketch(), lines[0]);
  CHECK_THAT(distance(g->start, g->end), WithinAbs(50.0, 1e-6));
  CHECK_FALSE(ed.is_free(lines[0]));
}

TEST_CASE("A conflicting dimension is kept, named and drawn, and the geometry stays", "[app][sketch]") {
  SketchEditor ed(sketch::Sketch(Id::random(), "Outline"), kSettings);
  ed.set_tool(SketchTool::Rectangle);
  ed.click({0, 0});
  ed.click({60, 40});
  const auto lines = of_kind(ed.sketch(), sketch::EntityKind::Line);
  ed.constrain(ConstraintKind::Fix, {ed.nearest_point({0, 0}, 1.0)});
  const Id w = ed.constrain(ConstraintKind::Distance, {lines[0]}, 60.0);
  ed.constrain(ConstraintKind::Distance, {lines[1]}, 40.0);
  REQUIRE(ed.status() == SolveStatus::Solved);
  const sketch::Sketch before = ed.sketch();

  // The opposite side can't be 70 when the first is 60 and both stay horizontal.
  const Id clash = ed.constrain(ConstraintKind::Distance, {lines[2]}, 70.0);
  REQUIRE_FALSE(clash.is_null());
  CHECK(ed.status() == SolveStatus::OverConstrained);
  CHECK(ed.is_conflicting(clash));
  const auto names = ed.conflict_labels();
  CHECK(std::find(names.begin(), names.end(), ed.sketch().label(clash)) != names.end());
  CHECK(std::find(names.begin(), names.end(), ed.sketch().label(w)) != names.end());
  CHECK(ed.status_text().rfind("Conflicting constraints: ", 0) == 0);
  // The geometry didn't move.
  for (const auto& e : before.entities()) {
    if (const auto p = before.point(e.id)) CHECK(*ed.sketch().point(e.id) == *p);
  }

  // Removing it solves again; undo brings the conflict back, redo removes it.
  REQUIRE(ed.remove({clash}));
  CHECK(ed.status() == SolveStatus::Solved);
  REQUIRE(ed.undo());
  CHECK(ed.status() == SolveStatus::OverConstrained);
  REQUIRE(ed.redo());
  CHECK(ed.status() == SolveStatus::Solved);
}

TEST_CASE("Chained lines snap to existing points and close a profile", "[app][sketch]") {
  SketchEditor ed(sketch::Sketch(), kSettings);
  ed.set_tool(SketchTool::Line);
  ed.click({0, 0});
  CHECK(ed.click({40, 0}).size() == 3);  // two points and a line
  CHECK(ed.chain_end() == ed.nearest_point({40, 0}, 0.1));
  // The chain goes on from (40, 0).
  CHECK(ed.click({20, 30}).size() == 2);
  // Closing on the first point reuses it and ends the chain.
  const Id first = ed.nearest_point({0.3, 0.2}, 1.0);
  REQUIRE_FALSE(first.is_null());
  CHECK(ed.click({0.3, 0.2}, first).size() == 1);
  CHECK(ed.chain_end().is_null());
  const auto& s = ed.sketch();
  CHECK(count(s, sketch::EntityKind::Line) == 3);
  CHECK(count(s, sketch::EntityKind::Point) == 3);
  // Only the first line is horizontal.
  CHECK(count(s, ConstraintKind::Horizontal) == 1);
  CHECK(profiles(s) == 1);
  // 3 points, one horizontal: 5 degrees of freedom.
  CHECK(ed.dof() == 5);
}

TEST_CASE("Circles, arcs and slots", "[app][sketch]") {
  SketchEditor ed(sketch::Sketch(), kSettings);
  ed.set_tool(SketchTool::Circle);
  ed.click({0, 0});
  ed.click({5, 0});
  const auto circles = of_kind(ed.sketch(), sketch::EntityKind::Circle);
  REQUIRE(circles.size() == 1);
  CHECK_THAT(sketch::circle_geometry(ed.sketch(), circles[0])->radius, WithinAbs(5.0, 1e-9));
  CHECK(ed.dof() == 3);

  ed.set_tool(SketchTool::Arc);
  ed.click({20, 0});
  ed.click({25, 0});
  ed.click({20, 9});  // the end lands on the start's radius
  const auto arcs = of_kind(ed.sketch(), sketch::EntityKind::Arc);
  REQUIRE(arcs.size() == 1);
  const auto arc = sketch::arc_geometry(ed.sketch(), arcs[0]);
  CHECK_THAT(arc->radius, WithinAbs(5.0, 1e-6));
  CHECK_THAT(arc->end_angle - arc->start_angle, WithinAbs(std::numbers::pi / 2, 1e-6));

  SketchEditor slot(sketch::Sketch(), kSettings);
  slot.set_tool(SketchTool::Slot);
  slot.click({0, 0});
  slot.click({30, 0});
  slot.click({30, 6});
  const auto& s = slot.sketch();
  CHECK(count(s, sketch::EntityKind::Arc) == 2);
  CHECK(count(s, sketch::EntityKind::Line) == 2);
  CHECK(count(s, ConstraintKind::Tangent) == 4);
  CHECK(count(s, ConstraintKind::Equal) == 1);
  INFO(slot.status_text());
  CHECK(slot.status() == SolveStatus::UnderConstrained);
  // Two centres and a radius.
  CHECK(slot.dof() == 5);
  CHECK(profiles(s) == 1);
  const auto p = sketch::extract_profiles(s, kSettings);
  // A 30 mm by 12 mm slot: rectangle plus a full circle.
  CHECK_THAT((*p)[0].area(), WithinAbs(30.0 * 12.0 + std::numbers::pi * 36.0, 1e-3));

  // Dimensioned: fully constrained.
  const auto caps = of_kind(s, sketch::EntityKind::Arc);
  const Id c1 = slot.nearest_point({0, 0}, 0.5), c2 = slot.nearest_point({30, 0}, 0.5);
  slot.constrain(ConstraintKind::Fix, {c1});
  slot.constrain(ConstraintKind::Horizontal, {c1, c2});
  slot.constrain(ConstraintKind::Distance, {c1, c2}, 40.0);
  slot.constrain(ConstraintKind::Diameter, {caps[0]}, 10.0);
  INFO(slot.status_text());
  CHECK(slot.status() == SolveStatus::Solved);
  CHECK_THAT(sketch::arc_geometry(slot.sketch(), caps[1])->radius, WithinAbs(5.0, 1e-6));
}

TEST_CASE("Constraints that fit a selection, and measured values", "[app][sketch]") {
  SketchEditor ed(sketch::Sketch(), kSettings);
  ed.set_tool(SketchTool::Rectangle);
  ed.click({0, 0});
  ed.click({30, 20});
  ed.set_tool(SketchTool::Circle);
  ed.click({15, 10});
  ed.click({18, 10});
  const auto lines = of_kind(ed.sketch(), sketch::EntityKind::Line);
  const Id circle = of_kind(ed.sketch(), sketch::EntityKind::Circle).at(0);
  const Id corner = ed.nearest_point({0, 0}, 0.5);
  const Id centre = ed.nearest_point({15, 10}, 0.5);

  using K = ConstraintKind;
  CHECK(ed.applicable({lines[0]}) == std::vector<K>{K::Horizontal, K::Vertical, K::Fix, K::Distance, K::Angle});
  CHECK(ed.applicable({circle}) == std::vector<K>{K::Fix, K::Radius, K::Diameter});
  CHECK(ed.applicable({lines[0], lines[1]}) == std::vector<K>{K::Parallel, K::Perpendicular, K::Equal, K::Angle});
  CHECK(ed.applicable({lines[0], circle}) == std::vector<K>{K::Tangent});
  CHECK(ed.applicable({corner, centre}) ==
        std::vector<K>{K::Coincident, K::Horizontal, K::Vertical, K::Distance, K::HorizontalDistance,
                       K::VerticalDistance});
  CHECK(ed.applicable({centre, lines[0]}) == std::vector<K>{K::PointOnCurve, K::Distance});
  CHECK(ed.applicable({corner, centre, lines[1]}) == std::vector<K>{K::Symmetric});
  CHECK(ed.applicable({corner, corner}).empty());

  CHECK_THAT(*ed.measure(K::HorizontalDistance, {corner, centre}), WithinAbs(15.0, 1e-9));
  CHECK_THAT(*ed.measure(K::VerticalDistance, {corner, centre}), WithinAbs(10.0, 1e-9));
  CHECK_THAT(*ed.measure(K::Diameter, {circle}), WithinAbs(6.0, 1e-9));
  CHECK_THAT(*ed.measure(K::Distance, {centre, lines[0]}), WithinAbs(10.0, 1e-9));
  CHECK_THAT(std::abs(*ed.measure(K::Angle, {lines[0], lines[1]})), WithinAbs(std::numbers::pi / 2, 1e-9));
  CHECK_FALSE(ed.measure(K::Radius, {lines[0]}));

  // A curve-then-point selection is reordered to the documented form.
  const Id on = ed.constrain(K::PointOnCurve, {lines[0], centre});
  REQUIRE_FALSE(on.is_null());
  CHECK(ed.sketch().find_constraint(on)->refs.front() == centre);
  // A kind that doesn't fit adds nothing.
  CHECK(ed.constrain(K::Radius, {lines[0]}).is_null());
}

TEST_CASE("Dimensions take expressions over the variables", "[app][sketch]") {
  double width = 25.0;
  auto lookup = [&](std::string_view name) -> std::optional<double> {
    if (name == "width") return width;
    return std::nullopt;
  };
  SketchEditor ed(sketch::Sketch(), kSettings, lookup);
  ed.set_tool(SketchTool::Line);
  ed.click({0, 0});
  ed.click({10, 0});
  ed.finish_tool();
  const Id line = of_kind(ed.sketch(), sketch::EntityKind::Line).at(0);
  const Id dim = ed.constrain(ConstraintKind::Distance, {line}, std::nullopt, "width * 2");
  REQUIRE_FALSE(dim.is_null());
  CHECK_THAT(ed.sketch().find_constraint(dim)->dimension->value, WithinAbs(50.0, 1e-9));
  const auto g = sketch::line_geometry(ed.sketch(), line);
  CHECK_THAT(distance(g->start, g->end), WithinAbs(50.0, 1e-6));

  // The variable changes: solving again follows it.
  width = 30.0;
  ed.solve();
  CHECK_THAT(ed.sketch().find_constraint(dim)->dimension->value, WithinAbs(60.0, 1e-9));

  CHECK_FALSE(ed.set_dimension(dim, 0.0, "height + 1").empty());  // unknown name: refused
  CHECK(ed.sketch().find_constraint(dim)->dimension->expression == "width * 2");
  CHECK(ed.set_dimension(dim, 12.5).empty());
  CHECK(ed.sketch().find_constraint(dim)->dimension->expression.empty());
  CHECK_THAT(ed.sketch().find_constraint(dim)->dimension->value, WithinAbs(12.5, 1e-9));
}

TEST_CASE("Dragging, deleting, construction geometry and undo", "[app][sketch]") {
  SketchEditor ed(sketch::Sketch(), kSettings);
  ed.set_tool(SketchTool::Rectangle);
  ed.click({0, 0});
  ed.click({20, 10});
  const Id far = ed.nearest_point({20, 10}, 0.5);
  REQUIRE(ed.drag(far, {30, 15}));
  // Still a rectangle, now 30 by 15.
  CHECK_THAT(ed.sketch().point(far)->x, WithinAbs(30.0, 1e-6));
  CHECK_THAT(ed.sketch().point(ed.nearest_point({30, 0}, 0.5))->y, WithinAbs(0.0, 1e-6));
  REQUIRE(ed.drag(far, {32, 16}, false));  // the same drag: one undo step
  REQUIRE(ed.undo());
  CHECK(*ed.sketch().point(far) == core::dvec2{20, 10});

  // Deleting a corner takes its two lines and their constraints with it.
  const Id corner = ed.nearest_point({20, 10}, 0.5);
  REQUIRE(ed.remove({corner}));
  CHECK(count(ed.sketch(), sketch::EntityKind::Line) == 2);
  CHECK(count(ed.sketch(), sketch::EntityKind::Point) == 3);
  CHECK(ed.sketch().validate().empty());
  CHECK(profiles(ed.sketch()) == 0);
  REQUIRE(ed.undo());
  CHECK(count(ed.sketch(), sketch::EntityKind::Line) == 4);

  // Construction lines leave the profile.
  const auto lines = of_kind(ed.sketch(), sketch::EntityKind::Line);
  REQUIRE(ed.toggle_construction({lines[0]}));
  CHECK(ed.sketch().find_entity(lines[0])->construction);
  CHECK(profiles(ed.sketch()) == 0);
  CHECK(ed.changed());

  // Nearest entity: points win, then curves.
  CHECK(ed.nearest_entity({0.2, 0.1}, 1.0) == ed.nearest_point({0, 0}, 0.5));
  CHECK(ed.nearest_entity({10, 0.3}, 1.0) == lines[0]);
  CHECK(ed.nearest_entity({10, 5}, 1.0).is_null());
}

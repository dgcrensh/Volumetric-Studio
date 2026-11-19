#include "parts.hpp"

#include <numbers>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/queries.hpp"

namespace parts {

using namespace vs::feature;
using vs::sketch::ConstraintKind;

Rect add_rectangle(Sketch& sketch, dvec2 lo, dvec2 hi) {
  Rect r;
  r.p[0] = sketch.add_point(lo);
  r.p[1] = sketch.add_point({hi.x, lo.y});
  r.p[2] = sketch.add_point(hi);
  r.p[3] = sketch.add_point({lo.x, hi.y});
  for (int i = 0; i < 4; ++i) r.l[i] = sketch.add_line(r.p[i], r.p[(i + 1) % 4]);
  return r;
}

void set_expression(Sketch& sketch, const Id& constraint, const std::string& expression) {
  auto* c = sketch.find_constraint(constraint);
  REQUIRE(c != nullptr);
  REQUIRE(c->dimension.has_value());
  c->dimension->expression = expression;
}

void constrain_rectangle(Sketch& sketch, const Rect& r, const std::string& width,
                         const std::string& height) {
  const auto w = *sketch.point(r.p[1]) - *sketch.point(r.p[0]);
  const auto h = *sketch.point(r.p[3]) - *sketch.point(r.p[0]);
  sketch.fix(r.p[0]);
  sketch.horizontal(r.l[0]);
  sketch.horizontal(r.l[2]);
  sketch.vertical(r.l[1]);
  sketch.vertical(r.l[3]);
  const Id cw = sketch.length(r.l[0], w.x);
  const Id ch = sketch.length(r.l[1], h.y);
  if (!width.empty()) set_expression(sketch, cw, width);
  if (!height.empty()) set_expression(sketch, ch, height);
}

namespace {
std::string num(double v) {
  std::ostringstream s;
  s << v;
  return s.str();
}
}  // namespace

Plate make_plate(double width, double thickness, double hole_d) {
  Plate p;
  auto& vars = p.tree.variables();
  p.width = vars.add("width", num(width) + " mm").value();
  p.thickness = vars.add("thickness", num(thickness)).value();
  p.hole_d = vars.add("hole_d", num(hole_d)).value();

  Sketch base(Id::random(), "Base");
  // Deliberately not at the final size: the solver gets it there.
  p.rect = add_rectangle(base, {0, 0}, {50, 20});
  constrain_rectangle(base, p.rect, "width", "width / 2");
  p.base_sketch = p.tree.add(SketchFeature{base, PrincipalPlane::XY});

  ExtrudeFeature extrude;
  extrude.sketch = p.base_sketch;
  extrude.depth = Param::expr("thickness");
  p.base = p.tree.add(extrude, "Plate");

  Sketch hole(Id::random(), "Hole");
  const Id origin = hole.add_point({0, 0}, /*construction=*/true);
  hole.fix(origin);
  const Id centre = hole.add_point({20, 10});
  const Id circle = hole.add_circle(centre, 5);
  const Id dx = hole.add_constraint(ConstraintKind::HorizontalDistance, {origin, centre}, 20.0);
  const Id dy = hole.add_constraint(ConstraintKind::VerticalDistance, {origin, centre}, 10.0);
  const Id dd = hole.diameter(circle, 10);
  set_expression(hole, dx, "width / 2");
  set_expression(hole, dy, "width / 4");
  set_expression(hole, dd, "hole_d");
  p.hole_sketch = p.tree.add(SketchFeature{hole, FaceRef{p.base, "end"}});

  ExtrudeFeature cut;
  cut.sketch = p.hole_sketch;
  cut.operation = Operation::Cut;
  cut.extent = Extent::ThroughAll;
  p.hole = p.tree.add(cut, "Hole");
  return p;
}

double plate_volume(double width, double thickness, double hole_d) {
  return width * (width / 2) * thickness -
         std::numbers::pi * (hole_d / 2) * (hole_d / 2) * thickness;
}

Tray make_tray(double wall, int holes) {
  Tray t;
  auto& vars = t.tree.variables();
  t.wall = vars.add("wall", num(wall) + " mm").value();
  t.holes = vars.add("holes", std::to_string(holes)).value();

  Sketch base(Id::random(), "Outline");
  const Rect r = add_rectangle(base, {0, 0}, {60, 40});
  constrain_rectangle(base, r);
  const Id base_sketch = t.tree.add(SketchFeature{base, PrincipalPlane::XY});
  ExtrudeFeature block;
  block.sketch = base_sketch;
  block.depth = Param::of(20);
  t.block = t.tree.add(block, "Block");

  t.shell = t.tree.add(ShellFeature{{FaceRef{t.block, "end"}}, Param::expr("wall"), false}, "Hollow");
  t.chamfer = t.tree.add(
      ChamferFeature{{EdgeRef{FaceRef{t.block, "start"}, std::nullopt}}, Param::of(1)},
      "Bottom chamfer");

  // The bottom face looks down (-Z); its frame (frame_for) has x = +X and
  // y = -Y, so model (15, 20) is sketch (15, -20). A cut on it goes up.
  Sketch hole(Id::random(), "Drain");
  hole.add_circle(dvec2{15, -20}, 2);
  const Id hole_sketch = t.tree.add(SketchFeature{hole, FaceRef{t.block, "start"}});
  ExtrudeFeature cut;
  cut.sketch = hole_sketch;
  cut.operation = Operation::Cut;
  cut.depth = Param::expr("wall + 1 mm");  // through the floor into the cavity
  t.hole = t.tree.add(cut, "Drain hole");

  LinearPatternFeature row;
  row.features = {t.hole};
  row.direction = PrincipalAxis::X;
  row.count = Param::expr("holes");
  row.spacing = Param::of(10);
  t.pattern = t.tree.add(row, "Drain holes");
  return t;
}

double tray_volume(double wall, int holes) {
  const double outer = 60.0 * 40.0 * 20.0;
  const double cavity = (60 - 2 * wall) * (40 - 2 * wall) * (20 - wall);
  // Four 45° strips along the bottom edges; neighbours overlap in a corner
  // piece of volume 1/3 (x + z < 1, y + z < 1), counted twice in the sum.
  const double chamfers = 0.5 * 1 * 1 * (2 * 60 + 2 * 40) - 4.0 / 3.0;
  return outer - cavity - chamfers - holes * std::numbers::pi * 4 * wall;
}

double volume(const vs::kernel::Solid& solid) {
  REQUIRE_FALSE(solid.is_empty());
  const auto m = vs::kernel::mass_properties(solid);
  REQUIRE(m.ok());
  return m->volume;
}

std::string codes(const vs::core::Diagnostics& diagnostics) {
  std::string out;
  for (const auto& d : diagnostics) {
    if (!out.empty()) out += ", ";
    out += d.code;
  }
  return out;
}

}  // namespace parts

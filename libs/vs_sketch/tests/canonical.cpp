#include "canonical.hpp"

#include <cmath>
#include <numbers>

namespace vs::sketch::test {

namespace {
constexpr double kPi = std::numbers::pi;

dvec2 polar(double r, double angle) { return {r * std::cos(angle), r * std::sin(angle)}; }
}  // namespace

Rectangle rectangle(double w, double h, bool dimensioned_width) {
  Rectangle r;
  Sketch& s = r.sketch;
  s.set_name("Rectangle");
  // A rough, slightly skewed guess.
  r.p00 = s.add_point({0.3, -0.2});
  r.p10 = s.add_point({w * 0.8, 0.4});
  r.p11 = s.add_point({w * 1.1, h * 0.9});
  r.p01 = s.add_point({-0.5, h * 1.2});
  r.bottom = s.add_line(r.p00, r.p10);
  r.right = s.add_line(r.p10, r.p11);
  r.top = s.add_line(r.p11, r.p01);
  r.left = s.add_line(r.p01, r.p00);
  r.h_bottom = s.horizontal(r.bottom);
  r.h_top = s.horizontal(r.top);
  r.v_right = s.vertical(r.right);
  r.v_left = s.vertical(r.left);
  // Fix the corner where it should be, then let the solver place the rest.
  s.set_point(r.p00, {0, 0});
  r.fix = s.fix(r.p00);
  if (dimensioned_width) r.width = s.length(r.bottom, w);
  r.height = s.length(r.left, h);
  return r;
}

Slot slot(double length, double radius) {
  Slot r;
  Sketch& s = r.sketch;
  s.set_name("Slot");
  r.c1 = s.add_point({0, 0});
  r.c2 = s.add_point({length * 0.9, 0.7});
  r.top_left = s.add_point({0.4, radius * 1.1});
  r.bottom_left = s.add_point({-0.3, -radius * 0.9});
  r.bottom_right = s.add_point({length * 1.05, -radius * 1.2});
  r.top_right = s.add_point({length * 0.95, radius * 0.8});
  // Left end runs counter-clockwise from the top to the bottom; right end
  // from the bottom to the top.
  r.left_arc = s.add_arc(r.c1, r.top_left, r.bottom_left);
  r.right_arc = s.add_arc(r.c2, r.bottom_right, r.top_right);
  r.bottom = s.add_line(r.bottom_left, r.bottom_right);
  r.top = s.add_line(r.top_right, r.top_left);
  s.fix(r.c1);
  s.add_constraint(ConstraintKind::Horizontal, {r.c1, r.c2});
  s.distance(r.c1, r.c2, length);
  s.radius(r.left_arc, radius);
  s.equal(r.left_arc, r.right_arc);
  s.tangent(r.top, r.left_arc);
  s.tangent(r.top, r.right_arc);
  s.tangent(r.bottom, r.left_arc);
  s.tangent(r.bottom, r.right_arc);
  return r;
}

Gear gear(int teeth, double root_r, double tip_r) {
  Gear r;
  Sketch& s = r.sketch;
  s.set_name("Gear");
  r.center = s.add_point({0, 0}, true);
  s.fix(r.center);
  const Id root = s.add_circle(r.center, root_r * 0.9, true);
  const Id tip = s.add_circle(r.center, tip_r * 1.1, true);
  s.radius(root, root_r);
  s.radius(tip, tip_r);

  // Per tooth (pitch P): root at 0, tip at 0.15 P, tip at 0.45 P, root at 0.6 P.
  const double pitch = 2 * kPi / teeth;
  const double fractions[4] = {0.0, 0.15, 0.45, 0.6};
  const bool on_tip[4] = {false, true, true, false};
  for (int t = 0; t < teeth; ++t) {
    for (int k = 0; k < 4; ++k) {
      const double angle = (t + fractions[k]) * pitch;
      const double radius = on_tip[k] ? tip_r : root_r;
      r.expected.push_back(polar(radius, angle));
      // Start a little off: 4% out and 0.02 rad round.
      const Id p = s.add_point(polar(radius * 1.04, angle + 0.02));
      r.points.push_back(p);
      s.point_on(p, on_tip[k] ? tip : root);
      const Id spoke = s.add_line(r.center, p, true);
      s.add_constraint(ConstraintKind::Angle, {spoke}, angle);
    }
  }
  for (std::size_t i = 0; i < r.points.size(); ++i) {
    s.add_line(r.points[i], r.points[(i + 1) % r.points.size()]);
  }
  return r;
}

BoltCircle bolt_circle(double plate_r, double pitch_r, int holes, double hole_d) {
  BoltCircle r;
  Sketch& s = r.sketch;
  s.set_name("Bolt circle");
  r.center = s.add_point({0, 0});
  s.fix(r.center);
  r.plate = s.add_circle(r.center, plate_r * 1.05);
  s.radius(r.plate, plate_r);
  r.pitch = s.add_circle(r.center, pitch_r * 0.95, true);
  s.radius(r.pitch, pitch_r);

  Id first_spoke;
  for (int k = 0; k < holes; ++k) {
    const double angle = 2 * kPi * k / holes;
    const Id c = s.add_point(polar(pitch_r * 1.03, angle + 0.05));
    r.hole_centers.push_back(c);
    r.holes.push_back(s.add_circle(c, hole_d * 0.6));
    s.point_on(c, r.pitch);
    const Id spoke = s.add_line(r.center, c, true);
    if (k == 0) {
      first_spoke = spoke;
      s.horizontal(spoke);
      s.diameter(r.holes[0], hole_d);
    } else {
      s.angle(first_spoke, spoke, angle);
      s.equal(r.holes[0], r.holes.back());
    }
  }
  return r;
}

bool same_geometry(const Sketch& a, const Sketch& b, double eps) {
  if (a.id() != b.id() || a.name() != b.name() || !(a.plane() == b.plane()) ||
      a.constraints() != b.constraints() || a.entities().size() != b.entities().size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.entities().size(); ++i) {
    Entity ea = a.entities()[i];
    Entity eb = b.entities()[i];
    if (auto* pa = ea.as<Point>(); pa != nullptr && eb.as<Point>() != nullptr) {
      if (vs::core::distance(pa->position, eb.as<Point>()->position) > eps) return false;
      eb.as<Point>()->position = pa->position;
    }
    if (auto* ca = ea.as<Circle>(); ca != nullptr && eb.as<Circle>() != nullptr) {
      if (std::abs(ca->radius - eb.as<Circle>()->radius) > eps) return false;
      eb.as<Circle>()->radius = ca->radius;
    }
    if (!(ea == eb)) return false;
  }
  return true;
}

}  // namespace vs::sketch::test

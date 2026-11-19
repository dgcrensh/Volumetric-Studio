// M12 field families: deterministic trigonometry, lattices, TPMS, thicken,
// clamp and field instancing, as fields and as nodes.

#include <cmath>
#include <numbers>
#include <random>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "scenes.hpp"
#include "vs/proc/evaluate.hpp"
#include "vs/proc/field.hpp"

using namespace vs;
using namespace vs::proc;
using core::dvec3;
using Catch::Approx;

namespace {

// Evaluates a graph whose output is `field` of node `source`.
GraphEvaluation run(Graph& g, const Id& source, const std::string& port = "field") {
  const Id out = g.add("output");
  g.link(source, port, out, "contents");
  EvaluateGraphOptions o;
  o.keep_outputs = true;
  return evaluate_graph(g, scenes::bare_inputs(), o);
}

bool infeasible_on(const core::Diagnostics& d, const std::string& port) {
  for (const auto& x : d.with_code("proc.node.infeasible")) {
    for (const auto& s : x->subjects) {
      if (s.label.ends_with("." + port)) return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("sin_det and cos_det match libm closely and repeat exactly", "[proc][field][infill]") {
  std::mt19937_64 rng(12);
  std::uniform_real_distribution<double> u(-20000.0, 20000.0);
  double worst = 0.0;
  for (int i = 0; i < 200000; ++i) {
    const double x = i < 1000 ? (i - 500) * std::numbers::pi / 8.0 : u(rng);
    worst = std::max(worst, std::abs(sin_det(x) - std::sin(x)));
    worst = std::max(worst, std::abs(cos_det(x) - std::cos(x)));
  }
  CHECK(worst < 1e-12);
  CHECK(sin_det(0.0) == 0.0);
  CHECK(cos_det(0.0) == 1.0);
  CHECK(sin_det(-1.25) == -sin_det(1.25));
  CHECK(std::isnan(sin_det(std::numeric_limits<double>::infinity())));
  // Bit patterns this build gives (fdlibm's kernels; no fused multiply-add).
  CHECK(sin_det(1.0) == Approx(0.8414709848078965).epsilon(1e-15));
  CHECK(cos_det(2.0) == Approx(-0.4161468365471424).epsilon(1e-15));
}

TEST_CASE("Strut lattices are distances to their cells' struts", "[proc][field][infill]") {
  const double cell = 10.0;
  const double t = 1.0;
  const dvec3 o{1, 2, 3};
  const Field cubic = lattice_field(LatticeCell::Cubic, cell, o, t);
  const Field bcc = lattice_field(LatticeCell::Bcc, cell, o, t);
  const Field octet = lattice_field(LatticeCell::Octet, cell, o, t);

  // On a strut: minus half the thickness.
  CHECK(cubic->eval(o + dvec3{3, 0, 0}) == Approx(-0.5));
  CHECK(bcc->eval(o + dvec3{5, 5, 5}) == Approx(-0.5));            // the body centre
  CHECK(octet->eval(o + dvec3{5, 5, 0}) == Approx(-0.5));          // a face centre
  CHECK(octet->eval(o + dvec3{2.5, 2.5, 0}) == Approx(-0.5));      // on a face diagonal
  // The cubic cell's centre is half a face diagonal from every edge.
  CHECK(cubic->eval(o + dvec3{5, 5, 5}) == Approx(5.0 * std::sqrt(2.0) - 0.5));
  // Periodic, in every direction.
  std::mt19937_64 rng(3);
  std::uniform_real_distribution<double> u(-30.0, 30.0);
  for (int i = 0; i < 200; ++i) {
    const dvec3 p{u(rng), u(rng), u(rng)};
    for (const Field& f : {cubic, bcc, octet}) {
      CHECK(f->eval(p + dvec3{cell, 0, 0}) == Approx(f->eval(p)).margin(1e-9));
      CHECK(f->eval(p + dvec3{0, -2 * cell, cell}) == Approx(f->eval(p)).margin(1e-9));
    }
  }
  // The nearest strut is in the point's own cell: compare with a brute
  // force over the neighbouring cells' struts for the BCC lattice.
  for (int i = 0; i < 200; ++i) {
    const dvec3 p{u(rng), u(rng), u(rng)};
    double best = 1e9;
    const auto seg = [](const dvec3& q, const dvec3& a, const dvec3& b) {
      const dvec3 ab = b - a;
      const double s = std::clamp(dot(q - a, ab) / dot(ab, ab), 0.0, 1.0);
      return length(q - (a + ab * s));
    };
    const dvec3 rel = (p - o) * (1.0 / cell);
    const dvec3 base{std::floor(rel.x), std::floor(rel.y), std::floor(rel.z)};
    for (int dx = -1; dx <= 1; ++dx) {
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
          const dvec3 c0 = o + (base + dvec3{double(dx), double(dy), double(dz)}) * cell;
          const dvec3 diag[4][2] = {{{0, 0, 0}, {1, 1, 1}}, {{1, 0, 0}, {0, 1, 1}},
                                    {{0, 1, 0}, {1, 0, 1}}, {{0, 0, 1}, {1, 1, 0}}};
          for (const auto& d : diag) best = std::min(best, seg(p, c0 + d[0] * cell, c0 + d[1] * cell));
        }
      }
    }
    CHECK(bcc->eval(p) == Approx(best - 0.5).margin(1e-9));
  }
}

TEST_CASE("Graded lattices take their thickness from a field", "[proc][field][infill]") {
  // Thickness grows with x: 0.5 mm at x = 0, 2.5 mm at x = 20.
  const Field grade = add_field(constant_field(0.5), scale_field(half_space_field({0, 0, 0}, {1, 0, 0}), 0.1));
  const Field f = lattice_field(LatticeCell::Cubic, 10.0, {0, 0, 0}, 1.0, grade);
  CHECK(f->eval({3, 0, 0}) == Approx(-0.5 * (0.5 + 0.3)));
  CHECK(f->eval({13, 0, 0}) == Approx(-0.5 * (0.5 + 1.3)));
  // Negative thickness clamps to none.
  CHECK(f->eval({-30, 0, 0}) == Approx(0.0));
  // The graded tree hashes differently from the plain one.
  CHECK(f->hash() != lattice_field(LatticeCell::Cubic, 10.0, {0, 0, 0}, 1.0)->hash());
}

TEST_CASE("TPMS sheets are centred on their surfaces", "[proc][field][infill]") {
  const double cell = 8.0;
  const double t = 0.8;
  const Field gyroid = tpms_field(TpmsSurface::Gyroid, cell, {0, 0, 0}, t);
  const Field p = tpms_field(TpmsSurface::SchwarzP, cell, {0, 0, 0}, t);
  const Field diamond = tpms_field(TpmsSurface::Diamond, cell, {0, 0, 0}, t);
  // Points on each surface: the sheet's middle.
  CHECK(gyroid->eval({0, 0, 0}) == Approx(-0.4).margin(1e-12));
  CHECK(p->eval({cell / 4, cell / 4, cell / 4}) == Approx(-0.4).margin(1e-12));
  CHECK(diamond->eval({0, 0, 0}) == Approx(-0.4).margin(1e-12));
  // Near the surface the value is close to the true distance: step along
  // the gradient from a surface point.
  const dvec3 g0 = normalize(dvec3{1, 1, 1});  // the gyroid's gradient at the origin
  CHECK(gyroid->eval(g0 * 0.3) == Approx(0.3 - 0.4).margin(0.01));
  // Periodic.
  std::mt19937_64 rng(5);
  std::uniform_real_distribution<double> u(-50.0, 50.0);
  for (int i = 0; i < 100; ++i) {
    const dvec3 q{u(rng), u(rng), u(rng)};
    for (const Field& f : {gyroid, p, diamond}) {
      CHECK(f->eval(q + dvec3{0, cell, 0}) == Approx(f->eval(q)).margin(1e-9));
    }
  }
  // Both sides of the sheet are outside it, the middle inside: a sheet,
  // not a solid network.
  CHECK(gyroid->eval(g0 * 2.0) > 0.0);
  CHECK(gyroid->eval(g0 * -2.0) > 0.0);
}

TEST_CASE("Instances place a template at every frame", "[proc][field][infill]") {
  FrameSet frames;
  core::ftransform a;
  a.translation = {10, 0, 0};
  core::ftransform b;
  b.translation = {-10, 0, 0};
  b.linear.cols = {core::fvec3{0, 1, 0}, core::fvec3{-1, 0, 0}, core::fvec3{0, 0, 1}};  // 90° about z
  frames.frames = {a, b};
  // A box 4 long in x, 2 wide: turned, it is long in y.
  const Field templ = box_field({0, 0, 0}, {4, 2, 2});
  const Field all = instances_field(templ, frames, 0.0);
  const Field skipping = instances_field(templ, frames, 3.0);
  CHECK(all->eval({11.5, 0, 0}) < 0.0);
  CHECK(all->eval({10, 1.5, 0}) > 0.0);
  CHECK(all->eval({-10, 1.5, 0}) < 0.0);   // turned
  CHECK(all->eval({-11.5, 0, 0}) > 0.0);
  std::mt19937_64 rng(9);
  std::uniform_real_distribution<double> u(-20.0, 20.0);
  for (int i = 0; i < 500; ++i) {
    const dvec3 q{u(rng), u(rng), u(rng)};
    // Skipping far copies never changes the answer.
    CHECK(skipping->eval(q) == all->eval(q));
    CHECK(all->eval(q) == std::min(templ->eval(q - dvec3{10, 0, 0}),
                                   templ->eval(dvec3{q.y, -(q.x + 10), q.z})));
  }
}

TEST_CASE("Infill nodes evaluate and check their inputs", "[proc][nodes][infill]") {
  SECTION("lattice") {
    Graph g;
    const Id l = g.add("field.lattice", "Lattice", {{"cell", Setting(std::string("octet"))}});
    const auto ev = run(g, l);
    INFO(scenes::codes(ev.diagnostics));
    REQUIRE(ev.ok());
    CHECK((*ev.contents)->op() == FieldOp::Lattice);
  }
  SECTION("graded lattice") {
    Graph g;
    const Id grade = g.add("field.constant", "Grade", {{"value", Setting(2.0)}});
    const Id l = g.add("field.lattice", "Lattice");
    g.link(grade, "field", l, "grade");
    const auto ev = run(g, l);
    REQUIRE(ev.ok());
    CHECK((*ev.contents)->eval({0, 0, 3}) == Approx(-1.0));
  }
  SECTION("thickness can't fill the cell") {
    Graph g;
    const Id l = g.add("field.lattice", "Lattice", {{"thickness", Setting(6.0)}});
    const auto ev = run(g, l);
    CHECK_FALSE(ev.ok());
    CHECK(infeasible_on(ev.diagnostics, "thickness"));
    CHECK(infeasible_on(ev.diagnostics, "cell_size"));
  }
  SECTION("tpms") {
    Graph g;
    const Id t = g.add("field.tpms", "Sheet", {{"surface", Setting(std::string("diamond"))}});
    const auto ev = run(g, t);
    REQUIRE(ev.ok());
    CHECK((*ev.contents)->eval({0, 0, 0}) == Approx(-0.5));
  }
  SECTION("an unknown TPMS is a setting error") {
    Graph g;
    const Id t = g.add("field.tpms", "Sheet", {{"surface", Setting(std::string("teapot"))}});
    const auto ev = run(g, t);
    CHECK_FALSE(ev.diagnostics.with_code("proc.graph.setting").empty());
  }
  SECTION("thicken and clamp") {
    Graph g;
    const Id plane = g.add("field.plane", "Plane");
    const Id thick = g.add("field.thicken", "Wall", {{"thickness", Setting(2.0)}});
    g.link(plane, "field", thick, "field");
    const Id clamp = g.add("field.clamp", "Clamp", {{"min", Setting(-0.5)}, {"max", Setting(3.0)}});
    g.link(thick, "field", clamp, "field");
    const auto ev = run(g, clamp);
    REQUIRE(ev.ok());
    CHECK((*ev.contents)->eval({0, 0, 0}) == Approx(-0.5));   // -1 clamped
    CHECK((*ev.contents)->eval({0, 0, 1.5}) == Approx(0.5));
    CHECK((*ev.contents)->eval({0, 0, 10}) == Approx(3.0));   // clamped
  }
  SECTION("instances of a field at a pattern") {
    Graph g;
    const Id ball = g.add("field.sphere", "Ball", {{"radius", Setting(1.0)}});
    const Id row = g.add("pattern.linear", "Row", {{"count", Setting(4.0)}, {"step", Setting(dvec3{5, 0, 0})}});
    const Id inst = g.add("field.instances", "Balls", {{"radius", Setting(1.0)}});
    g.link(ball, "field", inst, "template");
    g.link(row, "frames", inst, "frames");
    const auto ev = run(g, inst);
    REQUIRE(ev.ok());
    for (int i = 0; i < 4; ++i) CHECK((*ev.contents)->eval({5.0 * i, 0, 0}) == Approx(-1.0));
    CHECK((*ev.contents)->eval({20, 0, 0}) == Approx(4.0));
  }
}

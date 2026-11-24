// SVG import (S1, decision 0040): units, path commands, shapes, transforms,
// joints, open paths, smooth joints and Fix, and profiles from the result.

#include <cmath>
#include <numbers>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/kernel.hpp"
#include "vs/sketch/profile.hpp"
#include "vs/sketch/solver.hpp"
#include "vs/sketch/svg.hpp"

using namespace vs::sketch;
using Catch::Approx;
using vs::core::dvec2;

namespace {

constexpr double kPi = std::numbers::pi;
const vs::core::ProductSettings kSettings = vs::core::ProductSettings::defaults();

// An SVG document 100 mm square whose user units are millimetres.
std::string mm_doc(const std::string& body) {
  return R"svg(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="100mm" height="100mm" viewBox="0 0 100 100">)svg" +
         body + "</svg>";
}

struct Imported {
  Sketch sketch;
  SvgImport result;
};

Imported import(const std::string& svg, const SvgImportOptions& options = {}) {
  Imported out;
  auto r = import_svg(svg, out.sketch, options, kSettings);
  if (!r) FAIL(r.error().to_string());
  out.result = std::move(*r);
  CHECK(out.sketch.validate().empty());
  return out;
}

std::size_t count(const Sketch& s, EntityKind kind, bool construction = false) {
  std::size_t n = 0;
  for (const auto& e : s.entities()) {
    if (e.kind() == kind && e.construction == construction) ++n;
  }
  return n;
}

std::size_t constraints_of(const Sketch& s, ConstraintKind kind) {
  std::size_t n = 0;
  for (const auto& c : s.constraints()) {
    if (c.kind == kind) ++n;
  }
  return n;
}

double profile_area(const Sketch& s) {
  const auto profiles = extract_profiles(s, kSettings);
  if (!profiles) FAIL(profiles.error().to_string());
  double area = 0;
  for (const auto& p : *profiles) area += p.area();
  return area;
}

bool has_code(const vs::core::Diagnostics& d, std::string_view code) {
  return !d.with_code(code).empty();
}

// The point entity nearest `p`.
double nearest(const Sketch& s, dvec2 p) {
  double best = 1e300;
  for (const auto& e : s.entities()) {
    if (const auto* q = e.as<Point>()) best = std::min(best, vs::core::distance(q->position, p));
  }
  return best;
}

}  // namespace

TEST_CASE("SVG units set the scale and Y is flipped", "[sketch][svg]") {
  SECTION("width in mm through a viewBox") {
    auto i = import(mm_doc(R"svg(<rect x="10" y="20" width="30" height="40"/>)svg"));
    CHECK(count(i.sketch, EntityKind::Line) == 4);
    // y = 20..60 down from the top is 40..80 up from the bottom.
    CHECK(nearest(i.sketch, {10, 80}) < 1e-12);
    CHECK(nearest(i.sketch, {40, 40}) < 1e-12);
    CHECK(profile_area(i.sketch) == Approx(1200));
  }
  SECTION("a viewBox in other units") {
    auto i = import(R"svg(<svg width="2in" height="1in" viewBox="0 0 200 100">
      <rect width="100" height="100"/></svg>)svg");
    CHECK(profile_area(i.sketch) == Approx(25.4 * 25.4));
  }
  SECTION("bare numbers use pixels per inch") {
    const std::string doc = R"svg(<svg><rect x="0" y="0" width="96" height="96"/></svg>)svg";
    CHECK(profile_area(import(doc).sketch) == Approx(25.4 * 25.4));
    SvgImportOptions o;
    o.pixels_per_inch = 72;
    CHECK(profile_area(import(doc, o).sketch) == Approx(96 / 72.0 * 25.4 * 96 / 72.0 * 25.4));
    o.scale = 2;
    CHECK(profile_area(import(doc, o).sketch) == Approx(4 * std::pow(96 / 72.0 * 25.4, 2)));
  }
  SECTION("preserveAspectRatio centres a wider viewBox") {
    auto i = import(R"svg(<svg width="100mm" height="100mm" viewBox="0 0 200 100">
      <rect width="200" height="100"/></svg>)svg");
    // Scaled by a half and centred: y from 25 to 75.
    CHECK(nearest(i.sketch, {0, 25}) < 1e-12);
    CHECK(nearest(i.sketch, {100, 75}) < 1e-12);
  }
}

TEST_CASE("SVG path commands, absolute and relative", "[sketch][svg]") {
  SECTION("lines, H, V, implicit line-tos and compact numbers") {
    auto i = import(mm_doc(R"svg(<path d="M10,10 h20 v20 H10 Z
                                       m40-0 30.5.5-.5 19.5-30 0z"/>)svg"));
    // A 20 mm square, and (50, 10) (80.5, 10.5) (80, 30) (50, 30).
    CHECK(count(i.sketch, EntityKind::Line) == 8);
    CHECK(i.result.open.empty());
    CHECK(profile_area(i.sketch) == Approx(400 + 597.5));
  }
  SECTION("cubic and smooth cubic") {
    auto i = import(mm_doc(R"svg(<path d="M10 50 C10 30 30 30 30 50 S50 70 50 50 L10 50Z"/>)svg"));
    CHECK(count(i.sketch, EntityKind::Bezier) == 2);
    CHECK(count(i.sketch, EntityKind::Line) == 1);
    // S reflected the first curve's second handle about (30, 50).
    const auto* second = &i.sketch.entities().back();
    for (const auto& e : i.sketch.entities()) {
      if (e.kind() == EntityKind::Bezier) second = &e;
    }
    const auto g = bezier_geometry(i.sketch, second->id);
    REQUIRE(g);
    CHECK(g->p[1].x == Approx(30));
    CHECK(g->p[1].y == Approx(100 - 70));
  }
  SECTION("quadratics are raised exactly") {
    auto i = import(mm_doc(R"svg(<path d="M10 90 Q50 10 90 90 T170 90 Z"/>)svg"));
    CHECK(count(i.sketch, EntityKind::Bezier) == 2);
    for (const auto& e : i.sketch.entities()) {
      if (e.kind() != EntityKind::Bezier) continue;
      const auto g = bezier_geometry(i.sketch, e.id);
      // At t = 1/2 a quadratic is (p0 + 2q + p2) / 4.
      const dvec2 mid = g->at(0.5);
      if (g->p[0].x < 50) {
        CHECK(mid.x == Approx(50));
        CHECK(mid.y == Approx(100 - 50));
      } else {
        CHECK(mid.x == Approx(130));
        CHECK(mid.y == Approx(100 - 130));  // T reflected the control to (130, 170)
      }
    }
  }
  SECTION("arc flags run into the numbers after them") {
    auto i = import(mm_doc(R"svg(<path d="M30 50a20 20 0 1040 0z"/>)svg"));
    CHECK(i.result.diagnostics.empty());
    CHECK(count(i.sketch, EntityKind::Arc) == 1);
    CHECK(profile_area(i.sketch) == Approx(kPi * 400 / 2).epsilon(1e-3));
  }
  SECTION("bad path data keeps what came before") {
    auto i = import(mm_doc(R"svg(<path d="M10 10 L20 10 L20 20 Z M 30 30 L x"/>)svg"));
    CHECK(has_code(i.result.diagnostics, "sketch.import.path_data"));
    CHECK(count(i.sketch, EntityKind::Line) == 3);
  }
}

TEST_CASE("SVG arcs", "[sketch][svg]") {
  SECTION("a circular arc becomes a sketch arc") {
    // A half disc of radius 20 on a diameter from (30, 50) to (70, 50).
    auto i = import(mm_doc(R"svg(<path d="M30 50 A20 20 0 0 1 70 50 Z"/>)svg"));
    REQUIRE(count(i.sketch, EntityKind::Arc) == 1);
    CHECK_FALSE(has_code(i.result.diagnostics, "sketch.import.ellipse_approximated"));
    CHECK(profile_area(i.sketch) == Approx(kPi * 400 / 2).epsilon(1e-3));
    for (const auto& e : i.sketch.entities()) {
      if (const auto g = arc_geometry(i.sketch, e.id)) {
        CHECK(g->radius == Approx(20));
        CHECK(g->center.x == Approx(50));
        // Sweep 1 runs clockwise on screen (y down): over the top, which is up.
        CHECK(g->at((g->start_angle + g->end_angle) / 2).y == Approx(70));
      }
    }
  }
  SECTION("radii too small are scaled up") {
    auto i = import(mm_doc(R"svg(<path d="M30 50 A1 1 0 0 0 70 50 Z"/>)svg"));
    CHECK(profile_area(i.sketch) == Approx(kPi * 400 / 2).epsilon(1e-3));
  }
  SECTION("an elliptical arc is approximated within the minimum feature size") {
    auto i = import(mm_doc(R"svg(<path d="M10 50 A40 20 0 1 1 90 50 A40 20 0 1 1 10 50 Z"/>)svg"));
    CHECK(has_code(i.result.diagnostics, "sketch.import.ellipse_approximated"));
    CHECK(count(i.sketch, EntityKind::Arc) == 0);
    CHECK(count(i.sketch, EntityKind::Bezier) >= 4);
    CHECK(profile_area(i.sketch) == Approx(kPi * 40 * 20).epsilon(1e-4));
  }
  SECTION("a circle stretched by a transform is approximated too") {
    auto i = import(mm_doc(R"svg(<g transform="scale(2 1)"><circle cx="25" cy="50" r="10"/></g>)svg"));
    CHECK(count(i.sketch, EntityKind::Circle) == 0);
    CHECK(has_code(i.result.diagnostics, "sketch.import.ellipse_approximated"));
    CHECK(profile_area(i.sketch) == Approx(kPi * 20 * 10).epsilon(1e-4));
  }
}

TEST_CASE("SVG shapes", "[sketch][svg]") {
  SECTION("circle, and an ellipse that is round") {
    auto i = import(mm_doc(R"svg(<circle cx="50" cy="50" r="10"/><ellipse cx="20" cy="20" rx="5" ry="5"/>)svg"));
    CHECK(count(i.sketch, EntityKind::Circle) == 2);
    CHECK(profile_area(i.sketch) == Approx(kPi * 125));
  }
  SECTION("rounded rectangle") {
    auto i = import(mm_doc(R"svg(<rect x="10" y="10" width="60" height="40" rx="5"/>)svg"));
    CHECK(count(i.sketch, EntityKind::Line) == 4);
    CHECK(count(i.sketch, EntityKind::Arc) == 4);
    CHECK(profile_area(i.sketch) == Approx(60 * 40 - (4 - kPi) * 25).epsilon(1e-4));
  }
  SECTION("polygon closes, polyline and line stay open") {
    auto i = import(mm_doc(R"svg(<polygon points="10,10 50,10 30,40"/>
                              <polyline points="60,10 90,10 90,40"/>
                              <line x1="10" y1="90" x2="90" y2="90"/>)svg"));
    CHECK(count(i.sketch, EntityKind::Line) == 3);
    CHECK(count(i.sketch, EntityKind::Line, true) == 3);
    CHECK(i.result.open.size() == 3);
    CHECK(i.result.diagnostics.with_code("sketch.import.open").size() == 2);
    CHECK(profile_area(i.sketch) == Approx(0.5 * 40 * 30));
  }
  SECTION("text and images are skipped with a warning") {
    auto i = import(mm_doc(R"svg(<text x="10" y="10">Hi</text><image href="a.png"/>
                              <circle cx="50" cy="50" r="10"/>)svg"));
    CHECK(i.result.diagnostics.with_code("sketch.import.unsupported").size() == 2);
    CHECK(count(i.sketch, EntityKind::Circle) == 1);
  }
}

TEST_CASE("SVG groups, transforms, use and hidden elements", "[sketch][svg]") {
  auto i = import(mm_doc(R"svg(
    <defs><rect id="sq" width="10" height="10"/>
      <symbol id="sym" viewBox="0 0 1 1"><rect width="1" height="1"/></symbol></defs>
    <g transform="translate(10 10)">
      <g transform="rotate(90 5 5)"><use xlink:href="#sq"/></g>
      <use href="#sq" x="20"/>
      <use href="#sym" x="40" width="20" height="20"/>
    </g>
    <rect width="5" height="5" display="none"/>
    <g style="fill:red; display : none"><rect width="5" height="5"/></g>
    <svg x="70" y="70" width="20" height="20" viewBox="0 0 2 2"><rect width="2" height="2"/></svg>
    <use href="#missing"/>)svg"));
  CHECK(has_code(i.result.diagnostics, "sketch.import.reference"));
  // Two 10 mm squares, a 20 mm symbol and a 20 mm nested viewport.
  CHECK(profile_area(i.sketch) == Approx(100 + 100 + 400 + 400));
  CHECK(nearest(i.sketch, {10, 90}) < 1e-9);  // the rotated square about its own centre
  CHECK(nearest(i.sketch, {50, 70}) < 1e-9);  // the symbol's lower left corner
  CHECK(nearest(i.sketch, {70, 10}) < 1e-9);  // the nested svg's lower left corner
}

TEST_CASE("SVG ends within the gap join, across elements too", "[sketch][svg]") {
  SECTION("pieces of one outline") {
    auto i = import(mm_doc(R"svg(<path d="M10 10 L50 10"/><path d="M50.004 10 L50 50"/>
                              <path d="M50 50 L10 50 L10 10.003"/>)svg"));
    CHECK(i.result.open.empty());
    CHECK(count(i.sketch, EntityKind::Point) == 4);
    CHECK(profile_area(i.sketch) == Approx(1600).epsilon(1e-3));
  }
  SECTION("a wider gap is open, unless the option allows it") {
    const std::string doc = mm_doc(R"svg(<path d="M10 10 L50 10 L50 50 L10 50 L10 10.5"/>)svg");
    auto i = import(doc);
    CHECK(i.result.open.size() == 4);
    CHECK(has_code(i.result.diagnostics, "sketch.import.open"));
    CHECK(profile_area(i.sketch) == 0);
    SvgImportOptions o;
    o.gap_mm = 1;
    CHECK(import(doc, o).result.open.empty());
  }
  SECTION("a tail on a loop comes in as construction, the loop doesn't") {
    auto i = import(mm_doc(R"svg(<path d="M10 10 L50 10 L50 50 L10 50 Z M50 50 L80 80 L90 80"/>)svg"));
    CHECK(i.result.open.size() == 2);
    CHECK(count(i.sketch, EntityKind::Line) == 4);
    CHECK(profile_area(i.sketch) == Approx(1600));
    // The corner the tail starts from stays a real point.
    for (const auto& e : i.sketch.entities()) {
      if (const auto* p = e.as<Point>(); p && vs::core::distance(p->position, {50, 50}) < 1e-9) {
        CHECK_FALSE(e.construction);
      }
    }
  }
  SECTION("segments shorter than the gap are dropped") {
    auto i = import(mm_doc(R"svg(<path d="M10 10 L50 10 L50 10.001 L50 50 L10 50 Z"/>)svg"));
    CHECK(count(i.sketch, EntityKind::Line) == 4);
    CHECK(i.result.open.empty());
  }
}

TEST_CASE("SVG smooth joints become tangents, corners don't", "[sketch][svg]") {
  // A rounded slot: arcs meeting lines smoothly, and a Bézier bump on top
  // meeting a line smoothly at one end and at a corner at the other.
  const std::string doc = mm_doc(
      R"svg(<rect x="10" y="10" width="60" height="20" rx="10"/>
         <path d="M10 60 L40 60 C50 60 50 80 60 80 L60 60 L90 60 L90 90 L10 90 Z"/>)svg");
  auto i = import(doc);
  // The slot is 2 lines and 4 quarter arcs, each end's pair sharing a
  // centre: 4 smooth joints with the lines. The bump adds 1.
  CHECK(constraints_of(i.sketch, ConstraintKind::Tangent) == 4 + 1);
  CHECK(count(i.sketch, EntityKind::Point) == 6 + 2 + 7 + 2);
  const double before = profile_area(i.sketch);
  auto report = solve(i.sketch, kSettings);
  INFO(report.diagnostics.size());
  CHECK(report.status == SolveStatus::UnderConstrained);
  CHECK(report.redundant.empty());
  CHECK(profile_area(i.sketch) == Approx(before).epsilon(1e-9));

  SvgImportOptions o;
  o.smooth_joints = false;
  CHECK(constraints_of(import(doc, o).sketch, ConstraintKind::Tangent) == 0);

  SECTION("a circle drawn as arcs becomes a circle") {
    auto c = import(mm_doc(R"svg(<path d="M30 50 A20 20 0 0 1 70 50 A20 20 0 0 1 30 50 Z"/>
      <path d="M0 10 A5 5 0 0 0 5 5 A5 5 0 0 0 0 0 A5 5 0 0 0 -5 5 A5 5 0 0 0 0 10"/>)svg"));
    CHECK(count(c.sketch, EntityKind::Circle) == 2);
    CHECK(count(c.sketch, EntityKind::Arc) == 0);
    CHECK(c.result.open.empty());
    CHECK(profile_area(c.sketch) == Approx(kPi * (400 + 25)));
  }
}

TEST_CASE("SVG import with Fix holds everything without redundancy", "[sketch][svg]") {
  SvgImportOptions o;
  o.fix = true;
  auto i = import(mm_doc(R"svg(<rect x="10" y="10" width="60" height="20" rx="10"/>
                             <path d="M10 60 C10 40 40 40 40 60 Z"/>
                             <path d="M50 50 L90 50 A10 10 0 0 0 90 70 Z"/>
                             <circle cx="80" cy="85" r="5"/>
                             <path d="M60 95 L95 95"/>)svg"),
                  o);
  CHECK(constraints_of(i.sketch, ConstraintKind::Tangent) == 0);
  // Only the half circle needs its centre held: each end of the slot is two
  // arcs sharing a centre, held by their three fixed points.
  CHECK(constraints_of(i.sketch, ConstraintKind::HorizontalDistance) +
            constraints_of(i.sketch, ConstraintKind::VerticalDistance) ==
        1);
  const Sketch before = i.sketch;
  const auto report = solve(i.sketch, kSettings);
  CHECK(report.status == SolveStatus::Solved);
  CHECK(report.dof == 0);
  CHECK(report.redundant.empty());
  CHECK(report.conflicting.empty());
  for (const auto& e : before.entities()) {
    if (const auto* p = e.as<Point>()) {
      CHECK(vs::core::distance(*i.sketch.point(e.id), p->position) < 1e-9);
    }
  }
}

TEST_CASE("SVG import refuses what it can't read and leaves the sketch alone", "[sketch][svg]") {
  Sketch s;
  s.add_circle(dvec2{0, 0}, 5);
  const Sketch before = s;
  const auto code = [&](std::string_view svg, SvgImportOptions o = {}) {
    auto r = import_svg(svg, s, o, kSettings);
    REQUIRE_FALSE(r);
    CHECK(s == before);
    return r.error().code;
  };
  CHECK(code("<svg><path d='M0 0 L1 1'/>") == "sketch.import.malformed");
  CHECK(code("<html><body/></html>") == "sketch.import.malformed");
  CHECK(code("not xml at all") == "sketch.import.malformed");
  CHECK(code("<svg><text>only words</text></svg>") == "sketch.import.empty");
  SvgImportOptions bad;
  bad.pixels_per_inch = 0;
  CHECK(code("<svg/>", bad) == "sketch.import.option");
  Sketch t;
  const auto missing = import_svg_file("no/such/file.svg", t);
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == "sketch.import.file");
}

TEST_CASE("SVG import adds to a sketch that has geometry", "[sketch][svg]") {
  Sketch s;
  const Id existing = s.add_circle(dvec2{-50, -50}, 5);
  auto r = import_svg(mm_doc(R"svg(<circle cx="50" cy="50" r="10"/>)svg"), s, {}, kSettings);
  REQUIRE(r);
  CHECK(s.find_entity(existing) != nullptr);
  CHECK(r->curves.size() == 1);
  CHECK(count(s, EntityKind::Circle) == 2);
}

TEST_CASE("SVG fixture files import into profiles that make faces", "[sketch][svg]") {
  const std::string dir = std::string(VS_SKETCH_TEST_DATA) + "/svg/";
  SECTION("an Inkscape-style logo: an outline of curves round two holes") {
    Sketch s;
    auto r = import_svg_file(dir + "logo.svg", s, {}, kSettings);
    REQUIRE(r);
    CHECK(r->open.empty());
    const auto profiles = extract_profiles(s, kSettings);
    if (!profiles) FAIL(profiles.error().to_string());
    REQUIRE(profiles->size() == 1);
    CHECK((*profiles)[0].holes.size() == 2);
    const auto face = make_face(s, (*profiles)[0], kSettings);
    REQUIRE(face);
    CHECK(vs::kernel::check_validity(*face).empty());
    CHECK(vs::kernel::surface_area(*face) == Approx((*profiles)[0].area()).epsilon(1e-4));
    CHECK(solve(s, kSettings).ok());
  }
  SECTION("a drawing with an open stroke") {
    Sketch s;
    auto r = import_svg_file(dir + "open.svg", s, {}, kSettings);
    REQUIRE(r);
    CHECK_FALSE(r->open.empty());
    CHECK(has_code(r->diagnostics, "sketch.import.open"));
    const auto faces = make_faces(s, kSettings);
    REQUIRE(faces);
    CHECK(faces->size() == 1);
  }
}

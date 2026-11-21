// H2: profile bypasses that used to slip through extract_profiles().

#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vs/sketch/profile.hpp"
#include "vs/sketch/serialize.hpp"

using namespace vs::sketch;
using vs::core::Id;
using vs::core::ProductSettings;

namespace {

const ProductSettings kSettings = ProductSettings::defaults();

void rectangle(Sketch& s, dvec2 lo, dvec2 hi) {
  s.add_line(dvec2{lo.x, lo.y}, dvec2{hi.x, lo.y});
  s.add_line(dvec2{hi.x, lo.y}, dvec2{hi.x, hi.y});
  s.add_line(dvec2{hi.x, hi.y}, dvec2{lo.x, hi.y});
  s.add_line(dvec2{lo.x, hi.y}, dvec2{lo.x, lo.y});
}

}  // namespace

TEST_CASE("Two rectangles crossing in a '+' cross", "[sketch][profile][h2]") {
  // No corner of either lies inside the other: only their sides cross.
  Sketch s;
  rectangle(s, {-20, -5}, {20, 5});
  rectangle(s, {-5, -20}, {5, 20});
  const auto result = extract_profiles(s, kSettings);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == "sketch.profile.loops_cross");
}

TEST_CASE("A bowtie crosses itself", "[sketch][profile][h2]") {
  // Unequal lobes, so the signed area isn't zero (equal lobes are
  // "degenerate": they enclose no area overall).
  Sketch s;
  s.add_line(dvec2{0, 0}, dvec2{30, 10});
  s.add_line(dvec2{30, 10}, dvec2{30, 0});
  s.add_line(dvec2{30, 0}, dvec2{0, 4});
  s.add_line(dvec2{0, 4}, dvec2{0, 0});
  const auto result = extract_profiles(s, kSettings);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == "sketch.profile.self_crossing");
}

TEST_CASE("Coincident circles are one loop drawn twice", "[sketch][profile][h2]") {
  Sketch s;
  s.add_circle(dvec2{3, 4}, 7);
  s.add_circle(dvec2{3, 4}, 7);
  const auto result = extract_profiles(s, kSettings);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == "sketch.profile.loops_cross");
}

TEST_CASE("Loops that only touch still nest", "[sketch][profile][h2]") {
  SECTION("a rectangle against the side of another") {
    Sketch s;
    rectangle(s, {0, 0}, {10, 10});
    rectangle(s, {10, 2}, {20, 8});
    const auto result = extract_profiles(s, kSettings);
    REQUIRE(result);
    CHECK(result->size() == 2);
  }
  SECTION("a circle inside a circle, touching it") {
    Sketch s;
    s.add_circle(dvec2{0, 0}, 50);
    s.add_circle(dvec2{40, 0}, 10);
    const auto result = extract_profiles(s, kSettings);
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(result->front().holes.size() == 1);
  }
}

TEST_CASE("A hole near a large arc is inside it, not across it", "[sketch][profile][h2]") {
  // At 256 chords a turn a 900 mm circle's chords stray 0.068 mm inside it:
  // a small circle 0.03 mm inside the arc, between two chords' ends, used to
  // poke out of the polygon and be reported as crossing.
  constexpr double kPi = std::numbers::pi;
  constexpr double kBig = 900.0;
  constexpr double kSmall = 0.5;
  const double at = kPi / 256.0;  // half way between two of the old chords' ends
  const double centre = kBig - 0.03 - kSmall;
  Sketch s;
  s.add_circle(dvec2{0, 0}, kBig);
  s.add_circle(dvec2{centre * std::cos(at), centre * std::sin(at)}, kSmall);
  const auto result = extract_profiles(s, kSettings);
  REQUIRE(result);
  REQUIRE(result->size() == 1);
  CHECK(result->front().holes.size() == 1);
}

TEST_CASE("Many separate loops are compared only where their boxes meet",
          "[sketch][profile][h2]") {
  // 20 x 20 circles: 160,000 ordered pairs of 256-point polygons used to cost
  // about 10^10 distance tests; boxes leave only neighbours to compare.
  Sketch s;
  for (int i = 0; i < 20; ++i) {
    for (int j = 0; j < 20; ++j) s.add_circle(dvec2{i * 10.0, j * 10.0}, 4.0);
  }
  const auto result = extract_profiles(s, kSettings);
  REQUIRE(result);
  CHECK(result->size() == 400);
}

TEST_CASE("A sketch file can't give an object the null id", "[sketch][serialize][h2]") {
  const std::string kNull(32, '0');
  Sketch s;
  const Id p = s.add_point(dvec2{1, 2});
  const Id q = s.add_point(dvec2{3, 4});
  const Id c = s.add_constraint(ConstraintKind::Coincident, {p, q});
  const std::string text = to_json(s);
  REQUIRE(sketch_from_json(text));
  for (const Id& id : {s.id(), p, c}) {
    // Every occurrence: the object's own id and any references to it.
    std::string broken = text;
    for (auto at = broken.find(id.to_string()); at != std::string::npos;
         at = broken.find(id.to_string(), at)) {
      broken.replace(at, 32, kNull);
    }
    REQUIRE(broken != text);
    INFO(id.to_string());
    CHECK_FALSE(sketch_from_json(broken));
  }
}

#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "canonical.hpp"
#include "vs/sketch/serialize.hpp"
#include "vs/sketch/solver.hpp"

using namespace vs::sketch;

namespace {

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

}  // namespace

TEST_CASE("Sketches round-trip through JSON with their ids", "[sketch][json]") {
  auto r = test::slot(50, 8);
  r.sketch.set_plane({{1, 2, 3}, {0, 1, 0}, {0, 0, 1}});
  r.sketch.find_entity(r.top)->name = "Top";
  r.sketch.find_entity(r.c2)->construction = true;
  const Id d = r.sketch.length(r.top, 50);
  r.sketch.find_constraint(d)->dimension->expression = "slot_length";
  r.sketch.find_constraint(d)->name = "Slot length";
  // Values that need all 17 significant digits.
  r.sketch.set_point(r.c2, {0.1 + 0.2, 1.0 / 3.0});

  const std::string text = to_json(r.sketch);
  auto back = sketch_from_json(text);
  REQUIRE(back);
  CHECK(*back == r.sketch);  // exact, ids and all
  CHECK(to_json(*back) == text);  // stable output
}

TEST_CASE("A sketch read back solves the same way", "[sketch][json]") {
  auto r = test::bolt_circle(50, 35, 6, 8);
  auto back = sketch_from_json(to_json(r.sketch));
  REQUIRE(back);
  const auto settings = vs::core::ProductSettings::defaults();
  REQUIRE(solve(r.sketch, settings).ok());
  REQUIRE(solve(*back, settings).ok());
  CHECK(test::same_geometry(*back, r.sketch));
}

TEST_CASE("The checked-in sample sketch reads and solves", "[sketch][json]") {
  auto sketch = sketch_from_json(read_file(VS_SKETCH_TEST_DATA "/bracket.vsketch.json"));
  REQUIRE(sketch);
  CHECK(sketch->validate().empty());
  const auto report = solve(*sketch, vs::core::ProductSettings::defaults());
  CHECK(report.status == SolveStatus::Solved);
}

TEST_CASE("Malformed sketch files are reported by code", "[sketch][json]") {
  CHECK(sketch_from_json("{ not json").error().code == "sketch.json.syntax");
  CHECK(sketch_from_json("[]").error().code == "sketch.json.structure");
  CHECK(sketch_from_json(R"({"format": "other"})").error().code == "sketch.json.structure");

  auto r = test::rectangle(10, 5);
  std::string text = to_json(r.sketch);

  SECTION("an unsupported version") {
    text.replace(text.find("\"version\": 1"), 12, "\"version\": 9");
    CHECK(sketch_from_json(text).error().code == "sketch.json.version");
  }
  SECTION("an unknown constraint type") {
    text.replace(text.find("\"horizontal\""), 12, "\"sideways\"");
    CHECK(sketch_from_json(text).error().code == "sketch.json.structure");
  }
  SECTION("a bad id") {
    text.replace(text.find("\"id\": \"") + 7, 4, "zzzz");
    CHECK(sketch_from_json(text).error().code == "sketch.json.structure");
  }
}

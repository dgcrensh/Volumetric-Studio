#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vs/feature/expression.hpp"

using namespace vs::feature;
using Catch::Approx;

namespace {

double eval(std::string_view text, const Expression::Lookup& lookup = {}) {
  auto v = evaluate_expression(text, lookup);
  INFO(text);
  if (!v) FAIL(v.error().to_string());
  return *v;
}

std::string error_code(std::string_view text, const Expression::Lookup& lookup = {}) {
  auto v = evaluate_expression(text, lookup);
  return v ? std::string("ok") : v.error().code;
}

constexpr double kPi = std::numbers::pi;

}  // namespace

TEST_CASE("arithmetic follows the usual precedence", "[feature][expression]") {
  CHECK(eval("1 + 2 * 3") == 7);
  CHECK(eval("(1 + 2) * 3") == 9);
  CHECK(eval("10 - 4 - 3") == 3);
  CHECK(eval("24 / 4 / 3") == 2);
  CHECK(eval("2 ^ 3 ^ 2") == 512);  // right-associative
  CHECK(eval("-2 ^ 2") == -4);
  CHECK(eval("2 ^ -1") == 0.5);
  CHECK(eval("--3") == 3);
  CHECK(eval("+4") == 4);
  CHECK(eval(" 1.5e2 + .5 ") == 150.5);
  CHECK(eval("2E-3") == 0.002);
}

TEST_CASE("units convert into millimetres and radians", "[feature][expression]") {
  CHECK(eval("10 mm") == 10);
  CHECK(eval("10mm") == 10);
  CHECK(eval("2 cm") == 20);
  CHECK(eval("1.5 m") == 1500);
  CHECK(eval("250 um") == Approx(0.25));
  CHECK(eval("1 in") == 25.4);
  CHECK(eval("2 ft") == Approx(609.6));
  CHECK(eval("180 deg") == Approx(kPi));
  CHECK(eval("90\xC2\xB0") == Approx(kPi / 2));
  CHECK(eval("1 rad") == 1);
  CHECK(eval("1 in + 1 mm") == Approx(26.4));
  CHECK(eval("sin(30 deg)") == Approx(0.5));
}

TEST_CASE("built-in functions and pi", "[feature][expression]") {
  CHECK(eval("pi") == kPi);
  CHECK(eval("sqrt(16)") == 4);
  CHECK(eval("abs(-3)") == 3);
  CHECK(eval("min(4, 2, 8)") == 2);
  CHECK(eval("max(4, 2, 8)") == 8);
  CHECK(eval("pow(2, 10)") == 1024);
  CHECK(eval("hypot(3, 4)") == 5);
  CHECK(eval("atan2(1, 1)") == Approx(kPi / 4));
  CHECK(eval("floor(2.7) + ceil(2.2) + round(2.5)") == 8);
  CHECK(eval("ln(exp(2))") == Approx(2));
  CHECK(eval("log10(1000)") == Approx(3));
  CHECK(eval("cos(0) + tan(0) + asin(0) + acos(1) + atan(0)") == 1);
}

TEST_CASE("names are looked up and listed as references", "[feature][expression]") {
  const auto lookup = [](std::string_view name) -> std::optional<double> {
    if (name == "width") return 40.0;
    if (name == "t_2") return 3.0;
    return std::nullopt;
  };
  CHECK(eval("width / 2 + t_2", lookup) == 23);
  auto e = Expression::parse("width * width + t_2 - sin(width)");
  REQUIRE(e.ok());
  CHECK(e->references() == std::vector<std::string>{"t_2", "width"});
  CHECK_FALSE(e->is_constant());
  CHECK(e->text() == "width * width + t_2 - sin(width)");
  CHECK(Expression::parse("2 * pi")->is_constant());
  CHECK(error_code("height + 1", lookup) == "feature.expression.unknown_name");
}

TEST_CASE("a default expression is zero", "[feature][expression]") {
  const Expression e;
  CHECK(e.text() == "0");
  CHECK(e.is_constant());
  CHECK(*e.evaluate({}) == 0);
}

TEST_CASE("syntax errors are diagnostics with a position", "[feature][expression]") {
  for (const char* bad : {"", "1 +", "(1 + 2", "1 2", "2 * * 3", "1)", "3 width", "@", "1.2.3",
                          "mm", "2 * sin", "min(1,)"}) {
    INFO(bad);
    CHECK(error_code(bad) == "feature.expression.syntax");
  }
  auto r = Expression::parse("1 + * 2");
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().message.find("position 5") != std::string::npos);
}

TEST_CASE("unknown functions, wrong arity and non-finite results", "[feature][expression]") {
  CHECK(error_code("frob(2)") == "feature.expression.unknown_function");
  CHECK(error_code("sqrt(1, 2)") == "feature.expression.arity");
  CHECK(error_code("atan2(1)") == "feature.expression.arity");
  CHECK(error_code("max()") == "feature.expression.arity");
  CHECK(error_code("1 / 0") == "feature.expression.not_finite");
  CHECK(error_code("sqrt(-1)") == "feature.expression.not_finite");
  CHECK(error_code("ln(0)") == "feature.expression.not_finite");
}

TEST_CASE("variable names exclude built-ins", "[feature][expression]") {
  CHECK(is_valid_name("width"));
  CHECK(is_valid_name("_x2"));
  CHECK(is_valid_name("Hole_D"));
  CHECK_FALSE(is_valid_name(""));
  CHECK_FALSE(is_valid_name("2x"));
  CHECK_FALSE(is_valid_name("hole-d"));
  CHECK_FALSE(is_valid_name(std::string(65, 'a')));
  for (const char* reserved : {"pi", "sin", "max", "mm", "in", "deg", "rad"}) {
    INFO(reserved);
    CHECK(is_reserved_name(reserved));
    CHECK_FALSE(is_valid_name(reserved));
  }
}

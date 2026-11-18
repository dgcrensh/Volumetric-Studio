#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

#include "vs/core/units.hpp"

using namespace vs::core;
using Catch::Approx;

TEST_CASE("millimetres are the default unit", "[units]") {
  STATIC_REQUIRE(kDefaultLengthUnit == LengthUnit::Millimetre);
  STATIC_REQUIRE(to_mm(1.0, LengthUnit::Millimetre) == 1.0);
}

TEST_CASE("length conversions", "[units]") {
  CHECK(to_mm(1.0, LengthUnit::Inch) == 25.4);
  CHECK(to_mm(2.0, LengthUnit::Metre) == 2000.0);
  CHECK(to_mm(3.0, LengthUnit::Centimetre) == 30.0);
  CHECK(to_mm(1.0, LengthUnit::Foot) == 304.8);
  CHECK(to_mm(500.0, LengthUnit::Micrometre) == Approx(0.5));
  CHECK(from_mm(25.4, LengthUnit::Inch) == 1.0);
  for (auto u : {LengthUnit::Millimetre, LengthUnit::Centimetre, LengthUnit::Metre,
                 LengthUnit::Micrometre, LengthUnit::Inch, LengthUnit::Foot}) {
    CHECK(from_mm(to_mm(12.5, u), u) == Approx(12.5));
  }
}

TEST_CASE("parsing unit names", "[units]") {
  CHECK(parse_length_unit("mm") == LengthUnit::Millimetre);
  CHECK(parse_length_unit("MM") == LengthUnit::Millimetre);
  CHECK(parse_length_unit("Millimeter") == LengthUnit::Millimetre);
  CHECK(parse_length_unit(" in ") == LengthUnit::Inch);
  CHECK(parse_length_unit("\"") == LengthUnit::Inch);
  CHECK(parse_length_unit("\xC2\xB5m") == LengthUnit::Micrometre);
  CHECK(parse_length_unit("ft") == LengthUnit::Foot);
  CHECK_FALSE(parse_length_unit("furlong").has_value());
  CHECK_FALSE(parse_length_unit("").has_value());

  for (auto u : {LengthUnit::Millimetre, LengthUnit::Centimetre, LengthUnit::Metre,
                 LengthUnit::Micrometre, LengthUnit::Inch, LengthUnit::Foot}) {
    CHECK(parse_length_unit(symbol(u)) == u);
  }
}

TEST_CASE("angles", "[units]") {
  CHECK(degrees(180.0) == Approx(std::numbers::pi));
  CHECK(from_radians(std::numbers::pi / 2, AngleUnit::Degree) == Approx(90.0));
  CHECK(to_radians(1.0, AngleUnit::Radian) == 1.0);
}

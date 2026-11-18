#pragma once

// Units. All lengths inside the studio are stored in millimetres (the spec's
// default); other units appear only at the edges (input, display, import and
// export). Angles are stored in radians.

#include <numbers>
#include <optional>
#include <string_view>

namespace vs::core {

enum class LengthUnit {
  Millimetre,
  Centimetre,
  Metre,
  Micrometre,
  Inch,
  Foot,
};

inline constexpr LengthUnit kDefaultLengthUnit = LengthUnit::Millimetre;

// Millimetres per one of `unit`.
constexpr double millimetres_per(LengthUnit unit) {
  switch (unit) {
    case LengthUnit::Millimetre: return 1.0;
    case LengthUnit::Centimetre: return 10.0;
    case LengthUnit::Metre: return 1000.0;
    case LengthUnit::Micrometre: return 0.001;
    case LengthUnit::Inch: return 25.4;
    case LengthUnit::Foot: return 304.8;
  }
  return 1.0;
}

constexpr double to_mm(double value, LengthUnit unit) { return value * millimetres_per(unit); }
constexpr double from_mm(double millimetres, LengthUnit unit) {
  return millimetres / millimetres_per(unit);
}

// The short symbol, such as "mm" or "in".
std::string_view symbol(LengthUnit unit);

// Accepts symbols and names, case-insensitively: "mm", "millimetre",
// "millimeter", "cm", "m", "um", "µm", "in", "inch", "\"", "ft", "foot", "'".
std::optional<LengthUnit> parse_length_unit(std::string_view text);

enum class AngleUnit {
  Radian,
  Degree,
};

constexpr double to_radians(double value, AngleUnit unit) {
  return unit == AngleUnit::Degree ? value * (std::numbers::pi / 180.0) : value;
}
constexpr double from_radians(double radians, AngleUnit unit) {
  return unit == AngleUnit::Degree ? radians * (180.0 / std::numbers::pi) : radians;
}

constexpr double degrees(double value) { return to_radians(value, AngleUnit::Degree); }

}  // namespace vs::core

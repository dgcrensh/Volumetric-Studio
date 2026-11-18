#include "vs/core/units.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <utility>

namespace vs::core {

std::string_view symbol(LengthUnit unit) {
  switch (unit) {
    case LengthUnit::Millimetre: return "mm";
    case LengthUnit::Centimetre: return "cm";
    case LengthUnit::Metre: return "m";
    case LengthUnit::Micrometre: return "um";
    case LengthUnit::Inch: return "in";
    case LengthUnit::Foot: return "ft";
  }
  return "mm";
}

std::optional<LengthUnit> parse_length_unit(std::string_view text) {
  std::string key;
  key.reserve(text.size());
  for (const char ch : text) {
    if (ch == ' ' || ch == '\t') continue;
    key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  static const std::array<std::pair<std::string_view, LengthUnit>, 22> kNames{{
      {"mm", LengthUnit::Millimetre},
      {"millimetre", LengthUnit::Millimetre},
      {"millimeter", LengthUnit::Millimetre},
      {"millimetres", LengthUnit::Millimetre},
      {"millimeters", LengthUnit::Millimetre},
      {"cm", LengthUnit::Centimetre},
      {"centimetre", LengthUnit::Centimetre},
      {"centimeter", LengthUnit::Centimetre},
      {"m", LengthUnit::Metre},
      {"metre", LengthUnit::Metre},
      {"meter", LengthUnit::Metre},
      {"um", LengthUnit::Micrometre},
      {"\xC2\xB5m", LengthUnit::Micrometre},  // µm in UTF-8
      {"micrometre", LengthUnit::Micrometre},
      {"micrometer", LengthUnit::Micrometre},
      {"in", LengthUnit::Inch},
      {"inch", LengthUnit::Inch},
      {"inches", LengthUnit::Inch},
      {"\"", LengthUnit::Inch},
      {"ft", LengthUnit::Foot},
      {"foot", LengthUnit::Foot},
      {"'", LengthUnit::Foot},
  }};

  const auto it = std::find_if(kNames.begin(), kNames.end(),
                               [&](const auto& entry) { return entry.first == key; });
  if (it == kNames.end()) return std::nullopt;
  return it->second;
}

}  // namespace vs::core

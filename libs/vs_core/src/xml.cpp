#include "vs/core/xml.hpp"

#include <charconv>
#include <cstdint>
#include <system_error>

namespace vs::core::xml {

namespace {

// "#65" or "#x41": a numeric character reference's code point.
std::optional<char32_t> character_reference(std::string_view ent) {
  if (ent.size() < 2 || ent[0] != '#') return std::nullopt;
  int base = 10;
  ent.remove_prefix(1);
  if (ent[0] == 'x' || ent[0] == 'X') {
    base = 16;
    ent.remove_prefix(1);
  }
  std::uint32_t v = 0;
  const auto [end, ec] = std::from_chars(ent.data(), ent.data() + ent.size(), v, base);
  if (ec != std::errc() || end != ent.data() + ent.size() || ent.empty() || v == 0 ||
      v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) {
    return std::nullopt;
  }
  return static_cast<char32_t>(v);
}

void append_utf8(std::string& out, char32_t c) {
  const auto byte = [&](std::uint32_t b) { out.push_back(static_cast<char>(static_cast<unsigned char>(b))); };
  const auto v = static_cast<std::uint32_t>(c);
  if (v < 0x80) {
    byte(v);
  } else if (v < 0x800) {
    byte(0xc0 | (v >> 6));
    byte(0x80 | (v & 0x3f));
  } else if (v < 0x10000) {
    byte(0xe0 | (v >> 12));
    byte(0x80 | ((v >> 6) & 0x3f));
    byte(0x80 | (v & 0x3f));
  } else {
    byte(0xf0 | (v >> 18));
    byte(0x80 | ((v >> 12) & 0x3f));
    byte(0x80 | ((v >> 6) & 0x3f));
    byte(0x80 | (v & 0x3f));
  }
}

}  // namespace

std::string unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out.push_back(s[i]);
      continue;
    }
    const std::size_t semi = s.find(';', i);
    if (semi == std::string_view::npos) {
      out.push_back(s[i]);
      continue;
    }
    const std::string_view ent = s.substr(i + 1, semi - i - 1);
    if (ent == "lt") out.push_back('<');
    else if (ent == "gt") out.push_back('>');
    else if (ent == "amp") out.push_back('&');
    else if (ent == "quot") out.push_back('"');
    else if (ent == "apos") out.push_back('\'');
    else if (const std::optional<char32_t> c = character_reference(ent)) append_utf8(out, *c);
    else out.append(s.substr(i, semi - i + 1));
    i = semi;
  }
  return out;
}

std::string escape(std::string_view s) {
  std::string out;
  for (const char c : s) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

}  // namespace vs::core::xml

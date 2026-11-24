#pragma once

// A small XML reader: element tags and their attributes in document order,
// enough for 3MF (vs_mesh) and SVG (vs_sketch). Text content, comments,
// CDATA, processing instructions and declarations are skipped; there is no
// validation and no DTD. Moved from vs_mesh's 3MF reader (S1).

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vs::core::xml {

struct Tag {
  std::string_view name;  // the local name: any namespace prefix ("m:") left off
  bool closing = false;
  bool self_closing = false;
  std::vector<std::pair<std::string_view, std::string>> attributes;

  [[nodiscard]] std::optional<std::string_view> attribute(std::string_view key) const {
    for (const auto& [k, v] : attributes) {
      if (k == key) return std::string_view(v);
    }
    return std::nullopt;
  }
};

// Replaces the five predefined entities and numeric character references
// (written as UTF-8); anything else is left as it is.
std::string unescape(std::string_view s);

// Escapes <, >, & and " for an attribute value or text.
std::string escape(std::string_view s);

// Calls f(tag) for each element tag in document order. False when the text
// isn't well formed enough to scan.
template <typename F>
bool scan_xml(std::string_view xml, F&& f) {
  std::size_t i = 0;
  while (true) {
    const std::size_t lt = xml.find('<', i);
    if (lt == std::string_view::npos) return true;
    if (xml.substr(lt, 4) == "<!--") {
      const std::size_t e = xml.find("-->", lt);
      if (e == std::string_view::npos) return false;
      i = e + 3;
      continue;
    }
    if (xml.substr(lt, 9) == "<![CDATA[") {
      const std::size_t e = xml.find("]]>", lt);
      if (e == std::string_view::npos) return false;
      i = e + 3;
      continue;
    }
    if (xml.substr(lt, 2) == "<?" || xml.substr(lt, 2) == "<!") {
      const std::size_t e = xml.find('>', lt);
      if (e == std::string_view::npos) return false;
      i = e + 1;
      continue;
    }
    Tag tag;
    std::size_t p = lt + 1;
    if (p < xml.size() && xml[p] == '/') {
      tag.closing = true;
      ++p;
    }
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    const std::size_t name_start = p;
    while (p < xml.size() && !is_space(xml[p]) && xml[p] != '>' && xml[p] != '/') ++p;
    tag.name = xml.substr(name_start, p - name_start);
    // 3MF files may bind the core namespace to a prefix (<m:model>); the
    // elements are the same.
    if (const std::size_t colon = tag.name.find(':'); colon != std::string_view::npos) {
      tag.name.remove_prefix(colon + 1);
    }
    while (true) {
      while (p < xml.size() && is_space(xml[p])) ++p;
      if (p >= xml.size()) return false;
      if (xml[p] == '>') {
        ++p;
        break;
      }
      if (xml[p] == '/') {
        tag.self_closing = true;
        ++p;
        continue;
      }
      const std::size_t k = p;
      while (p < xml.size() && xml[p] != '=' && !is_space(xml[p]) && xml[p] != '>') ++p;
      const std::string_view key = xml.substr(k, p - k);
      while (p < xml.size() && is_space(xml[p])) ++p;
      if (p >= xml.size() || xml[p] != '=') return false;
      ++p;
      while (p < xml.size() && is_space(xml[p])) ++p;
      if (p >= xml.size() || (xml[p] != '"' && xml[p] != '\'')) return false;
      const char q = xml[p++];
      const std::size_t v = p;
      p = xml.find(q, p);
      if (p == std::string_view::npos) return false;
      tag.attributes.emplace_back(key, unescape(xml.substr(v, p - v)));
      ++p;
    }
    if (!f(tag)) return true;
    i = p;
  }
}

}  // namespace vs::core::xml

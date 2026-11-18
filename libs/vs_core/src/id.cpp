#include "vs/core/id.hpp"

#include <array>
#include <random>

namespace vs::core {

Id Id::random() {
  // std::random_device is the OS entropy source on MSVC, libstdc++ and libc++.
  thread_local std::random_device device;
  const auto word = [] {
    std::uint64_t w = 0;
    for (int i = 0; i < 2; ++i) {
      w = (w << 32) | static_cast<std::uint64_t>(device());
    }
    return w;
  };
  Id id;
  do {
    id = Id(word(), word());
  } while (id.is_null());
  return id;
}

Id Id::from_hash(const Hash256& hash) { return Id(hash.word(0), hash.word(1)); }

Id Id::derive(const Id& parent, std::string_view name) {
  Hasher h;
  h.add(std::string_view("vs.id.derive.name")).add(parent).add(name);
  return from_hash(h.finalize());
}

Id Id::derive(const Id& parent, std::uint64_t index) {
  Hasher h;
  h.add(std::string_view("vs.id.derive.index")).add(parent).add(index);
  return from_hash(h.finalize());
}

std::string Id::to_string() const {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(32, '0');
  for (int i = 0; i < 16; ++i) {
    out[static_cast<std::size_t>(15 - i)] = kDigits[(hi >> (4 * i)) & 0xf];
    out[static_cast<std::size_t>(31 - i)] = kDigits[(lo >> (4 * i)) & 0xf];
  }
  return out;
}

std::optional<Id> Id::parse(std::string_view text) {
  if (text.size() != 32) return std::nullopt;
  std::array<std::uint64_t, 2> words{};
  for (std::size_t i = 0; i < 32; ++i) {
    const char c = text[i];
    std::uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint64_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<std::uint64_t>(c - 'A' + 10);
    } else {
      return std::nullopt;
    }
    auto& w = words[i / 16];
    w = (w << 4) | digit;
  }
  return Id(words[0], words[1]);
}

}  // namespace vs::core

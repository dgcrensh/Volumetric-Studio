#pragma once

// Stable 128-bit identifiers for every authored object (volumes, connection
// surfaces, features, sketch entities, nodes, ...).
//
// An Id is stable because it is stored, never recomputed from content: once
// assigned it survives renames, edits and rebuilds. The text form is 32
// lowercase hex digits, high word first, and round-trips exactly.
//
// Two ways to make one:
//   - Id::random() for objects a user authors (uses the OS entropy source).
//   - Id::derive(parent, name) for objects a procedure generates. It is a
//     pure function of its inputs, so regenerating a volume yields the same
//     child IDs every time (spec, design rule 7).

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "vs/core/hash.hpp"

namespace vs::core {

struct Id {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;

  constexpr Id() = default;
  constexpr Id(std::uint64_t high, std::uint64_t low) : hi(high), lo(low) {}

  static constexpr Id null() { return {}; }
  [[nodiscard]] constexpr bool is_null() const { return hi == 0 && lo == 0; }

  // A fresh random ID. Never null.
  static Id random();

  // A deterministic ID for something named `name` under `parent`.
  static Id derive(const Id& parent, std::string_view name);
  static Id derive(const Id& parent, std::uint64_t index);

  // The first 128 bits of a digest.
  static Id from_hash(const Hash256& hash);

  [[nodiscard]] std::string to_string() const;
  // Accepts exactly 32 hex digits (either case); nullopt otherwise.
  static std::optional<Id> parse(std::string_view text);

  friend constexpr bool operator==(const Id&, const Id&) = default;
  friend constexpr auto operator<=>(const Id&, const Id&) = default;
};

inline void hash_append(Hasher& h, const Id& id) { h.add(id.hi).add(id.lo); }

}  // namespace vs::core

template <>
struct std::hash<vs::core::Id> {
  std::size_t operator()(const vs::core::Id& id) const noexcept {
    // Random and derived IDs are already uniformly distributed.
    return static_cast<std::size_t>(id.hi ^ (id.lo * 0x9e3779b97f4a7c15ull));
  }
};

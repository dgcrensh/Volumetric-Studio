#pragma once

// Deterministic hashing (spec: design rule 7, evaluation-engine cache keys).
//
// The Hasher feeds BLAKE3 with an explicit, platform-independent byte stream:
//
//   - bool           1 byte, 0 or 1
//   - integers       widened to 64 bits (signed or unsigned), little endian,
//                    so `long` hashes the same on Windows (32-bit) and Linux
//                    (64-bit)
//   - double         IEEE-754 binary64 bits, little endian, 8 bytes
//   - float          IEEE-754 binary32 bits, little endian, 4 bytes
//   - strings        length as a 64-bit integer, then the raw bytes
//   - enums          their underlying value, as an integer
//
// Floating-point values are canonicalised first: -0.0 hashes as +0.0 (they
// compare equal, so equal values give equal hashes) and every NaN hashes as
// the single quiet NaN. FP64 and FP32 values never hash alike, because they
// write different byte counts; the precision split is part of identity.
//
// Composite types provide `hash_append(Hasher&, const T&)` next to their
// definition, found by argument-dependent lookup from Hasher::add.

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace vs::core {

static_assert(std::numeric_limits<double>::is_iec559, "IEEE-754 doubles required");
static_assert(std::numeric_limits<float>::is_iec559, "IEEE-754 floats required");

// A 256-bit BLAKE3 digest.
struct Hash256 {
  std::array<std::uint8_t, 32> bytes{};

  [[nodiscard]] std::string to_hex() const;
  // The digest read as four little-endian 64-bit words (index 0 to 3), for
  // compact keys such as std::hash.
  [[nodiscard]] std::uint64_t word(std::size_t index) const;

  friend bool operator==(const Hash256&, const Hash256&) = default;
  friend auto operator<=>(const Hash256&, const Hash256&) = default;
};

class Hasher {
 public:
  Hasher();
  ~Hasher();
  Hasher(Hasher&&) noexcept;
  Hasher& operator=(Hasher&&) noexcept;
  Hasher(const Hasher&);
  Hasher& operator=(const Hasher&);

  // Raw bytes, exactly as given. Prefer the typed add() overloads.
  Hasher& add_bytes(std::span<const std::byte> bytes);
  Hasher& add_bytes(const void* data, std::size_t size);

  // Hashes any value that has a hash_append overload.
  template <typename T>
  Hasher& add(const T& value);

  // The digest of everything added so far. Does not reset the hasher.
  [[nodiscard]] Hash256 finalize() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

// --- primitive encodings ------------------------------------------------

void hash_append(Hasher& h, bool value);
void hash_append(Hasher& h, double value);
void hash_append(Hasher& h, float value);
void hash_append(Hasher& h, std::string_view value);
void hash_append(Hasher& h, const std::string& value);
void hash_append(Hasher& h, const char* value);

namespace detail {
void append_u64(Hasher& h, std::uint64_t value);
void append_i64(Hasher& h, std::int64_t value);
}  // namespace detail

template <std::integral T>
  requires(!std::same_as<T, bool>)
void hash_append(Hasher& h, T value) {
  if constexpr (std::is_signed_v<T>) {
    detail::append_i64(h, static_cast<std::int64_t>(value));
  } else {
    detail::append_u64(h, static_cast<std::uint64_t>(value));
  }
}

template <typename E>
  requires std::is_enum_v<E>
void hash_append(Hasher& h, E value) {
  hash_append(h, static_cast<std::underlying_type_t<E>>(value));
}

inline void hash_append(Hasher& h, const Hash256& value) {
  h.add_bytes(value.bytes.data(), value.bytes.size());
}

// Canonical bit patterns used by the float encodings, exposed for tests.
[[nodiscard]] std::uint64_t canonical_bits(double value);
[[nodiscard]] std::uint32_t canonical_bits(float value);

template <typename T>
Hasher& Hasher::add(const T& value) {
  hash_append(*this, value);
  return *this;
}

// Convenience: the digest of a sequence of values.
template <typename... Ts>
[[nodiscard]] Hash256 hash_of(const Ts&... values) {
  Hasher h;
  (h.add(values), ...);
  return h.finalize();
}

// The BLAKE3 digest of raw bytes, with no encoding applied.
[[nodiscard]] Hash256 blake3_of_bytes(std::span<const std::byte> bytes);

}  // namespace vs::core

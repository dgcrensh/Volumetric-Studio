#include "vs/core/hash.hpp"

#include <blake3.h>

#include <cmath>
#include <cstring>

namespace vs::core {

struct Hasher::State {
  blake3_hasher hasher;
};

Hasher::Hasher() : state_(std::make_unique<State>()) {
  blake3_hasher_init(&state_->hasher);
}

Hasher::~Hasher() = default;
Hasher::Hasher(Hasher&&) noexcept = default;
Hasher& Hasher::operator=(Hasher&&) noexcept = default;

Hasher::Hasher(const Hasher& other) : state_(std::make_unique<State>(*other.state_)) {}

Hasher& Hasher::operator=(const Hasher& other) {
  if (this != &other) {
    state_ = std::make_unique<State>(*other.state_);
  }
  return *this;
}

Hasher& Hasher::add_bytes(std::span<const std::byte> bytes) {
  blake3_hasher_update(&state_->hasher, bytes.data(), bytes.size());
  return *this;
}

Hasher& Hasher::add_bytes(const void* data, std::size_t size) {
  blake3_hasher_update(&state_->hasher, data, size);
  return *this;
}

Hash256 Hasher::finalize() const {
  Hash256 out;
  blake3_hasher_finalize(&state_->hasher, out.bytes.data(), out.bytes.size());
  return out;
}

namespace {

template <typename U>
void append_le(Hasher& h, U value) {
  static_assert(std::is_unsigned_v<U>);
  std::array<std::uint8_t, sizeof(U)> buf{};
  for (std::size_t i = 0; i < sizeof(U); ++i) {
    buf[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu);
  }
  h.add_bytes(buf.data(), buf.size());
}

}  // namespace

namespace detail {

void append_u64(Hasher& h, std::uint64_t value) { append_le(h, value); }

void append_i64(Hasher& h, std::int64_t value) {
  append_le(h, static_cast<std::uint64_t>(value));
}

}  // namespace detail

std::uint64_t canonical_bits(double value) {
  if (std::isnan(value)) {
    return 0x7ff8000000000000ull;
  }
  if (value == 0.0) {
    return 0;  // folds -0.0 into +0.0
  }
  return std::bit_cast<std::uint64_t>(value);
}

std::uint32_t canonical_bits(float value) {
  if (std::isnan(value)) {
    return 0x7fc00000u;
  }
  if (value == 0.0f) {
    return 0;
  }
  return std::bit_cast<std::uint32_t>(value);
}

void hash_append(Hasher& h, bool value) {
  const std::uint8_t byte = value ? 1 : 0;
  h.add_bytes(&byte, 1);
}

void hash_append(Hasher& h, double value) { append_le(h, canonical_bits(value)); }

void hash_append(Hasher& h, float value) { append_le(h, canonical_bits(value)); }

void hash_append(Hasher& h, std::string_view value) {
  detail::append_u64(h, value.size());
  h.add_bytes(value.data(), value.size());
}

void hash_append(Hasher& h, const std::string& value) {
  hash_append(h, std::string_view(value));
}

void hash_append(Hasher& h, const char* value) { hash_append(h, std::string_view(value)); }

std::string Hash256::to_hex() const {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::uint8_t b : bytes) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0x0f]);
  }
  return out;
}

std::uint64_t Hash256::word(std::size_t index) const {
  std::uint64_t value = 0;
  const std::size_t base = (index % 4) * 8;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[base + i]) << (8 * i);
  }
  return value;
}

Hash256 blake3_of_bytes(std::span<const std::byte> bytes) {
  Hasher h;
  h.add_bytes(bytes);
  return h.finalize();
}

}  // namespace vs::core

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "vs/core/hash.hpp"
#include "vs/core/vec.hpp"

using namespace vs::core;

TEST_CASE("BLAKE3 matches the reference vector for empty input", "[hash]") {
  // From the BLAKE3 reference test vectors (input_len 0).
  CHECK(blake3_of_bytes({}).to_hex() ==
        "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}

TEST_CASE("hashing is deterministic and order sensitive", "[hash]") {
  CHECK(hash_of(1, 2.0, std::string("x")) == hash_of(1, 2.0, std::string("x")));
  CHECK(hash_of(1, 2) != hash_of(2, 1));
}

TEST_CASE("finalize does not reset the hasher", "[hash]") {
  Hasher h;
  h.add(42);
  const Hash256 first = h.finalize();
  CHECK(h.finalize() == first);
  h.add(43);
  CHECK(h.finalize() != first);
  CHECK(h.finalize() == hash_of(42, 43));
}

TEST_CASE("copied hashers continue independently", "[hash]") {
  Hasher a;
  a.add(std::string_view("prefix"));
  Hasher b = a;
  a.add(1);
  b.add(2);
  CHECK(a.finalize() == hash_of(std::string_view("prefix"), 1));
  CHECK(b.finalize() == hash_of(std::string_view("prefix"), 2));
}

TEST_CASE("integers hash by value, independent of their C++ type", "[hash]") {
  // All integers are widened to 64 bits, so `long` (32-bit on Windows,
  // 64-bit on Linux) hashes the same everywhere.
  CHECK(hash_of(std::int32_t{7}) == hash_of(std::int64_t{7}));
  CHECK(hash_of(7L) == hash_of(7LL));
  CHECK(hash_of(std::uint8_t{7}) == hash_of(std::uint64_t{7}));
  CHECK(hash_of(std::size_t{7}) == hash_of(std::uint64_t{7}));
  // Signed values are two's complement in 64 bits.
  CHECK(hash_of(std::int8_t{-1}) == hash_of(std::uint64_t{0xffffffffffffffffull}));
}

TEST_CASE("floating-point values are canonicalised", "[hash]") {
  CHECK(hash_of(0.0) == hash_of(-0.0));
  CHECK(hash_of(0.0f) == hash_of(-0.0f));

  const double qnan = std::numeric_limits<double>::quiet_NaN();
  const double other_nan = std::bit_cast<double>(0x7ff0000000000001ull);  // signalling payload
  const double neg_nan = -qnan;
  CHECK(hash_of(qnan) == hash_of(other_nan));
  CHECK(hash_of(qnan) == hash_of(neg_nan));
  CHECK(canonical_bits(other_nan) == 0x7ff8000000000000ull);
  CHECK(canonical_bits(std::numeric_limits<float>::signaling_NaN()) == 0x7fc00000u);

  CHECK(hash_of(1.0) != hash_of(std::nextafter(1.0, 2.0)));
  CHECK(hash_of(std::numeric_limits<double>::infinity()) !=
        hash_of(-std::numeric_limits<double>::infinity()));
}

TEST_CASE("FP32 and FP64 values never hash alike", "[hash]") {
  CHECK(hash_of(1.0f) != hash_of(1.0));
  CHECK(hash_of(fvec3{1, 2, 3}) != hash_of(dvec3{1, 2, 3}));
}

TEST_CASE("strings are length prefixed", "[hash]") {
  CHECK(hash_of(std::string("ab"), std::string("c")) !=
        hash_of(std::string("a"), std::string("bc")));
  CHECK(hash_of(std::string("abc")) == hash_of(std::string_view("abc")));
  CHECK(hash_of("abc") == hash_of(std::string_view("abc")));
  CHECK(hash_of(std::string()) != hash_of(std::string(1, '\0')));
}

TEST_CASE("enums hash as their underlying value", "[hash]") {
  enum class Colour : std::uint8_t { Red = 3 };
  CHECK(hash_of(Colour::Red) == hash_of(3u));
}

TEST_CASE("Hash256 helpers", "[hash]") {
  Hash256 h;
  for (std::size_t i = 0; i < h.bytes.size(); ++i) h.bytes[i] = static_cast<std::uint8_t>(i);
  CHECK(h.to_hex() == "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  CHECK(h.word(0) == 0x0706050403020100ull);
  CHECK(h.word(1) == 0x0f0e0d0c0b0a0908ull);
  CHECK(h.word(3) == 0x1f1e1d1c1b1a1918ull);
}

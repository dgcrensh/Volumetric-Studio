// Golden hashes: the same values must hash identically on every platform and
// compiler, forever. These digests were recorded once and checked in; CI runs
// this file on Windows (MSVC) and Linux (GCC and Clang).
//
// If one of these fails, the hash encoding changed. That invalidates every
// cache key ever written, so it needs a deliberate decision (and a bump of
// kEngineVersion), never a quick update of the expected value.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "vs/core/core.hpp"

using namespace vs::core;

namespace {

struct GoldenCase {
  std::string_view name;
  Hash256 actual;
  std::string_view expected;
};

// Built from literal values only. Anything computed through libm (sin, cos)
// may differ in the last bit between platforms, which would test the maths
// library rather than the hash encoding.
dtransform sample_transform() {
  dtransform t;
  t.linear = dmat3::from_columns({0.6, 0.8, 0.0}, {-0.8, 0.6, 0.0}, {0.0, 0.0, 2.0});
  t.translation = {10.5, -20.25, 3.0};
  return t;
}

}  // namespace

TEST_CASE("golden hashes are identical on every platform", "[hash][golden]") {
  const GoldenCase cases[] = {
      {"empty", Hasher().finalize(),
       "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
      {"bool", hash_of(true, false),
       "687376c930d7020a32f04c396fc2e5eab49cd09a738fa03d573033416a6a47ce"},
      {"integers", hash_of(0, 1, -1, std::int64_t{1} << 40, std::uint64_t{0xfedcba9876543210ull}),
       "6988b127142fe20bc4697716186330bf325ddf032f96a5cf65a65eb2aaec8f17"},
      {"doubles",
       hash_of(0.0, 1.0, -1.5, 3.141592653589793, 1e-6, 1000.0,
               std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max()),
       "f3ec321d79c7380627cbc23c6d2e5c67737f223b88914b9108f64f271d18ab74"},
      {"double specials",
       hash_of(-0.0, std::numeric_limits<double>::infinity(),
               -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()),
       "ffd650061c5469b47c79142d744617b8cfd833f005f69b197be57e3632e0c5cc"},
      {"floats", hash_of(0.0f, 1.0f, -1.5f, 0.1f, std::numeric_limits<float>::denorm_min()),
       "3a8af4452e9855b7774408cec085682964a33204749bfa34f386bb9d957c8b1c"},
      {"strings", hash_of(std::string_view(""), std::string_view("volume"),
                          std::string_view("\xC2\xB5m")),
       "f1047a3fd7f262688743099bcd4b754ecb0b99369dc60fa14598d50ba6f55e3d"},
      {"dvec3", hash_of(dvec3{1.0, -2.0, 0.1}),
       "d4bd29ba3f0bf375dcd131d73473db66665014eab0980cfc5b3e7ca1c3611670"},
      {"fvec3", hash_of(fvec3{1.0f, -2.0f, 0.1f}),
       "c7f0b3fedc30271a1d7e89b0a99b21c617cdd55c63048710bbcf50d7d5d8123c"},
      {"dtransform", hash_of(sample_transform()),
       "b645d01b6f5944280588c69c7242ae4ae77d7e545be748bf41027fba46ea420b"},
      {"dbox3", hash_of(dbox3::from_min_max({-1, -2, -3}, {4, 5, 6}), dbox3::empty()),
       "1e03fc3b8b3435b2b40056477708e71c750dc0d0fcee1353335a6f4f09feefca"},
      {"id", hash_of(Id(0x0123456789abcdefull, 0xfedcba9876543210ull)),
       "1640eaddd0b1ec57f30285858f5552c02714eb98b210c74a9fe085b70000f86e"},
      {"derived id", hash_of(Id::derive(Id(1, 2), "child"), Id::derive(Id(1, 2), 7u)),
       "e7fa9669aabe54a892f54f72d1755c343319a6a3386764fb00bdeed321de9534"},
      {"settings", hash_of(ProductSettings::defaults()),
       "9eb515250157607a1f82d0448b4e766ba41b158ff895729e42c5823441b9af9b"},
  };

  for (const auto& c : cases) {
    INFO("case: " << c.name << "\n  actual:   " << c.actual.to_hex());
    CHECK(c.actual.to_hex() == c.expected);
  }
}

TEST_CASE("derived IDs are golden too", "[id][golden]") {
  // Procedures rely on derived child IDs being identical across platforms.
  CHECK(Id::derive(Id(1, 2), "child").to_string() == "db0055ee2174e42ef93131f1146acb1b");
  CHECK(Id::derive(Id::null(), 0u).to_string() == "958fd2da8a147920b7b5f1060e5953a9");
}

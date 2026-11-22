// The GPU's double-precision arithmetic against the CPU's, bit for bit:
// what L1's "match the CPU hashes" rests on. Skips (with a message) where
// there is no GPU device.

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "device.hpp"
#include "glsl.hpp"

using namespace vs::gpu;

namespace {

// out[i] = op(a[i], b[i]) for each op, as doubles, plus float(a[i]); the
// square root is the one every field shader uses (arithmetic_glsl()).
const char* kHeader = R"(#version 430
#define P precise
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer A { double a[]; };
layout(std430, binding = 1) readonly buffer B { double b[]; };
layout(std430, binding = 2) writeonly buffer O { double o[]; };
layout(std430, binding = 3) writeonly buffer F { float fo[]; };
uniform uint n;
)";
const char* kMain = R"(
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= n) return;
  precise double x = a[i];
  precise double y = b[i];
  precise double add = x + y;
  precise double sub = x - y;
  precise double mul = x * y;
  precise double div = x / y;
  precise double sq = sqrt_rn(abs(x));
  precise double fl = floor(x);
  precise double mad = x * y + x;
  o[8 * i + 0] = add;
  o[8 * i + 1] = sub;
  o[8 * i + 2] = mul;
  o[8 * i + 3] = div;
  o[8 * i + 4] = sq;
  o[8 * i + 5] = fl;
  o[8 * i + 6] = mad;
  o[8 * i + 7] = 1.0 / y;
  fo[i] = float(x);
}
)";

}  // namespace

TEST_CASE("GPU double arithmetic matches the CPU bit for bit", "[gpu][arithmetic]") {
  Device device;
  if (!device.ok()) {
    WARN("No GPU device: " << device.why());
    return;
  }
  INFO(device.description());

  constexpr std::uint32_t n = 1 << 22;
  std::mt19937_64 rng(42);
  std::vector<double> a(n), b(n);
  std::uniform_real_distribution<double> mantissa(1.0, 2.0);
  std::uniform_int_distribution<int> exponent(-60, 60);
  for (std::uint32_t i = 0; i < n; ++i) {
    a[i] = std::ldexp(mantissa(rng), exponent(rng)) * ((rng() & 1) ? -1.0 : 1.0);
    b[i] = std::ldexp(mantissa(rng), exponent(rng)) * ((rng() & 1) ? -1.0 : 1.0);
  }
  // Rounding cases for float(): halfway values in FP32.
  a[0] = 1.0 + std::ldexp(1.0, -24);
  a[1] = 1.0 + 3.0 * std::ldexp(1.0, -24);
  // H6: subnormals and tiny normals, where the residual test of sqrt_rn()
  // underflowed: every exponent from the smallest subnormal up to 2^-880.
  for (std::uint32_t i = 0; i < 4096; ++i) {
    const int e = -1074 + static_cast<int>(i % 195);
    a[2 + i] = std::ldexp(mantissa(rng), e) * ((i & 1) ? -1.0 : 1.0);
  }
  a[4100] = std::numeric_limits<double>::denorm_min();
  a[4101] = std::numeric_limits<double>::min();
  a[4102] = std::nextafter(std::numeric_limits<double>::min(), 0.0);
  a[4103] = std::ldexp(1.0, -900);
  a[4104] = std::nextafter(std::ldexp(1.0, -900), 0.0);

  const std::string shader = std::string(kHeader) + arithmetic_glsl() + kMain;
  std::vector<double> out(8 * static_cast<std::size_t>(n));
  std::vector<float> fout(n);
  std::string log;
  device.run([&](const gl::Functions& f) {
    const gl::GLuint program = device.program(f, shader, log);
    if (program == 0) return;
    Buffer ba(f, n * sizeof(double), a.data());
    Buffer bb(f, n * sizeof(double), b.data());
    Buffer bo(f, out.size() * sizeof(double));
    Buffer bf(f, n * sizeof(float));
    ba.bind(0);
    bb.bind(1);
    bo.bind(2);
    bf.bind(3);
    f.UseProgram(program);
    f.Uniform1ui(f.GetUniformLocation(program, "n"), n);
    f.DispatchCompute(n / 64, 1, 1);
    f.MemoryBarrier(gl::kBufferUpdateBarrierBit);
    bo.read(out.data(), out.size() * sizeof(double));
    bf.read(fout.data(), fout.size() * sizeof(float));
  });
  INFO(log);
  REQUIRE(log.empty());

  const char* names[8] = {"add", "sub", "mul", "div", "sqrt", "floor", "mul-add", "reciprocal"};
  std::size_t mismatches[9] = {};
  for (std::uint32_t i = 0; i < n; ++i) {
    const double x = a[i], y = b[i];
    const double expect[8] = {x + y, x - y, x * y, x / y, std::sqrt(std::abs(x)), std::floor(x),
                              x * y + x, 1.0 / y};
    for (std::uint32_t k = 0; k < 8; ++k) {
      if (std::bit_cast<std::uint64_t>(expect[k]) != std::bit_cast<std::uint64_t>(out[8 * i + k])) {
        ++mismatches[k];
      }
    }
    if (std::bit_cast<std::uint32_t>(static_cast<float>(x)) != std::bit_cast<std::uint32_t>(fout[i])) {
      ++mismatches[8];
    }
  }
  for (int k = 0; k < 8; ++k) {
    INFO(names[k]);
    CHECK(mismatches[k] == 0);
  }
  INFO("float()");
  CHECK(mismatches[8] == 0);
}

#pragma once

// Small fixed-size vectors.
//
// The precision split from the spec lives in these types: authored geometry
// (boundaries, connection surfaces, transforms, sketches) uses the FP64
// aliases dvec2/dvec3/dvec4; generated meshes, fields and display use the
// FP32 aliases fvec2/fvec3/fvec4. There are no implicit conversions between
// the two. Use to_fp32() and to_fp64(), so every narrowing is visible.

#include <cmath>
#include <cstddef>

#include "vs/core/hash.hpp"

namespace vs::core {

template <typename T>
struct Vec2 {
  T x{}, y{};

  constexpr Vec2() = default;
  constexpr Vec2(T x_, T y_) : x(x_), y(y_) {}

  constexpr T& operator[](std::size_t i) { return i == 0 ? x : y; }
  constexpr const T& operator[](std::size_t i) const { return i == 0 ? x : y; }

  friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
};

template <typename T>
struct Vec3 {
  T x{}, y{}, z{};

  constexpr Vec3() = default;
  constexpr Vec3(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}

  constexpr T& operator[](std::size_t i) { return i == 0 ? x : (i == 1 ? y : z); }
  constexpr const T& operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : z); }

  friend constexpr bool operator==(const Vec3&, const Vec3&) = default;
};

template <typename T>
struct Vec4 {
  T x{}, y{}, z{}, w{};

  constexpr Vec4() = default;
  constexpr Vec4(T x_, T y_, T z_, T w_) : x(x_), y(y_), z(z_), w(w_) {}
  constexpr Vec4(const Vec3<T>& v, T w_) : x(v.x), y(v.y), z(v.z), w(w_) {}

  constexpr T& operator[](std::size_t i) {
    return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
  }
  constexpr const T& operator[](std::size_t i) const {
    return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
  }
  [[nodiscard]] constexpr Vec3<T> xyz() const { return {x, y, z}; }

  friend constexpr bool operator==(const Vec4&, const Vec4&) = default;
};

using dvec2 = Vec2<double>;
using dvec3 = Vec3<double>;
using dvec4 = Vec4<double>;
using fvec2 = Vec2<float>;
using fvec3 = Vec3<float>;
using fvec4 = Vec4<float>;

// --- arithmetic ---------------------------------------------------------

#define VS_CORE_VEC_BINARY_OP(OP)                                              \
  template <typename T>                                                        \
  constexpr Vec2<T> operator OP(const Vec2<T>& a, const Vec2<T>& b) {          \
    return {a.x OP b.x, a.y OP b.y};                                           \
  }                                                                            \
  template <typename T>                                                        \
  constexpr Vec3<T> operator OP(const Vec3<T>& a, const Vec3<T>& b) {          \
    return {a.x OP b.x, a.y OP b.y, a.z OP b.z};                               \
  }                                                                            \
  template <typename T>                                                        \
  constexpr Vec4<T> operator OP(const Vec4<T>& a, const Vec4<T>& b) {          \
    return {a.x OP b.x, a.y OP b.y, a.z OP b.z, a.w OP b.w};                   \
  }

VS_CORE_VEC_BINARY_OP(+)
VS_CORE_VEC_BINARY_OP(-)
VS_CORE_VEC_BINARY_OP(*)
VS_CORE_VEC_BINARY_OP(/)
#undef VS_CORE_VEC_BINARY_OP

template <typename T>
constexpr Vec2<T> operator*(const Vec2<T>& v, T s) { return {v.x * s, v.y * s}; }
template <typename T>
constexpr Vec3<T> operator*(const Vec3<T>& v, T s) { return {v.x * s, v.y * s, v.z * s}; }
template <typename T>
constexpr Vec4<T> operator*(const Vec4<T>& v, T s) { return {v.x * s, v.y * s, v.z * s, v.w * s}; }
template <typename T>
constexpr Vec2<T> operator*(T s, const Vec2<T>& v) { return v * s; }
template <typename T>
constexpr Vec3<T> operator*(T s, const Vec3<T>& v) { return v * s; }
template <typename T>
constexpr Vec4<T> operator*(T s, const Vec4<T>& v) { return v * s; }
template <typename T>
constexpr Vec2<T> operator/(const Vec2<T>& v, T s) { return {v.x / s, v.y / s}; }
template <typename T>
constexpr Vec3<T> operator/(const Vec3<T>& v, T s) { return {v.x / s, v.y / s, v.z / s}; }
template <typename T>
constexpr Vec4<T> operator/(const Vec4<T>& v, T s) { return {v.x / s, v.y / s, v.z / s, v.w / s}; }

template <typename T>
constexpr Vec2<T> operator-(const Vec2<T>& v) { return {-v.x, -v.y}; }
template <typename T>
constexpr Vec3<T> operator-(const Vec3<T>& v) { return {-v.x, -v.y, -v.z}; }
template <typename T>
constexpr Vec4<T> operator-(const Vec4<T>& v) { return {-v.x, -v.y, -v.z, -v.w}; }

template <typename T>
constexpr Vec2<T>& operator+=(Vec2<T>& a, const Vec2<T>& b) { return a = a + b; }
template <typename T>
constexpr Vec3<T>& operator+=(Vec3<T>& a, const Vec3<T>& b) { return a = a + b; }
template <typename T>
constexpr Vec4<T>& operator+=(Vec4<T>& a, const Vec4<T>& b) { return a = a + b; }
template <typename T>
constexpr Vec2<T>& operator-=(Vec2<T>& a, const Vec2<T>& b) { return a = a - b; }
template <typename T>
constexpr Vec3<T>& operator-=(Vec3<T>& a, const Vec3<T>& b) { return a = a - b; }
template <typename T>
constexpr Vec4<T>& operator-=(Vec4<T>& a, const Vec4<T>& b) { return a = a - b; }

// --- geometric functions ------------------------------------------------

template <typename T>
constexpr T dot(const Vec2<T>& a, const Vec2<T>& b) { return a.x * b.x + a.y * b.y; }
template <typename T>
constexpr T dot(const Vec3<T>& a, const Vec3<T>& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
template <typename T>
constexpr T dot(const Vec4<T>& a, const Vec4<T>& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// The z component of the 3D cross product of (a, 0) and (b, 0).
template <typename T>
constexpr T cross(const Vec2<T>& a, const Vec2<T>& b) { return a.x * b.y - a.y * b.x; }

template <typename T>
constexpr Vec3<T> cross(const Vec3<T>& a, const Vec3<T>& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

template <typename V>
auto length_squared(const V& v) { return dot(v, v); }

template <typename V>
auto length(const V& v) { return std::sqrt(dot(v, v)); }

template <typename V>
auto distance(const V& a, const V& b) { return length(a - b); }

// Returns the zero vector for a zero-length input rather than NaNs.
template <typename V>
V normalize(const V& v) {
  const auto len = length(v);
  if (len == decltype(len){0}) {
    return V{};
  }
  return v / len;
}

template <typename T>
constexpr Vec3<T> min(const Vec3<T>& a, const Vec3<T>& b) {
  return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}
template <typename T>
constexpr Vec3<T> max(const Vec3<T>& a, const Vec3<T>& b) {
  return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}
template <typename T>
constexpr Vec2<T> min(const Vec2<T>& a, const Vec2<T>& b) {
  return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y};
}
template <typename T>
constexpr Vec2<T> max(const Vec2<T>& a, const Vec2<T>& b) {
  return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y};
}

// Component-wise comparison within an absolute tolerance.
template <typename T>
bool approx_equal(const Vec3<T>& a, const Vec3<T>& b, T tolerance) {
  return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
         std::abs(a.z - b.z) <= tolerance;
}
template <typename T>
bool approx_equal(const Vec2<T>& a, const Vec2<T>& b, T tolerance) {
  return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance;
}

// --- explicit precision conversions -------------------------------------

constexpr fvec2 to_fp32(const dvec2& v) {
  return {static_cast<float>(v.x), static_cast<float>(v.y)};
}
constexpr fvec3 to_fp32(const dvec3& v) {
  return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}
constexpr fvec4 to_fp32(const dvec4& v) {
  return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z),
          static_cast<float>(v.w)};
}
constexpr dvec2 to_fp64(const fvec2& v) { return {v.x, v.y}; }
constexpr dvec3 to_fp64(const fvec3& v) { return {v.x, v.y, v.z}; }
constexpr dvec4 to_fp64(const fvec4& v) { return {v.x, v.y, v.z, v.w}; }

// --- hashing ------------------------------------------------------------

template <typename T>
void hash_append(Hasher& h, const Vec2<T>& v) {
  h.add(v.x).add(v.y);
}
template <typename T>
void hash_append(Hasher& h, const Vec3<T>& v) {
  h.add(v.x).add(v.y).add(v.z);
}
template <typename T>
void hash_append(Hasher& h, const Vec4<T>& v) {
  h.add(v.x).add(v.y).add(v.z).add(v.w);
}

}  // namespace vs::core

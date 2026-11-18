#pragma once

// 3x3 and 4x4 matrices, column major: `m[c]` is column c and `m(r, c)` is the
// element in row r, column c. Matrices multiply column vectors on the right,
// so `a * b` applies b first, then a.
//
// FP64 aliases (dmat3, dmat4) are for authored geometry; FP32 aliases (fmat3,
// fmat4) are for display and generated data. Convert with to_fp32/to_fp64.

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

#include "vs/core/hash.hpp"
#include "vs/core/vec.hpp"

namespace vs::core {

template <typename T>
struct Mat3 {
  std::array<Vec3<T>, 3> cols{Vec3<T>{1, 0, 0}, Vec3<T>{0, 1, 0}, Vec3<T>{0, 0, 1}};

  static constexpr Mat3 identity() { return {}; }
  static constexpr Mat3 zero() {
    Mat3 m;
    m.cols = {Vec3<T>{}, Vec3<T>{}, Vec3<T>{}};
    return m;
  }
  static constexpr Mat3 from_columns(const Vec3<T>& c0, const Vec3<T>& c1, const Vec3<T>& c2) {
    Mat3 m;
    m.cols = {c0, c1, c2};
    return m;
  }
  static constexpr Mat3 diagonal(const Vec3<T>& d) {
    return from_columns({d.x, 0, 0}, {0, d.y, 0}, {0, 0, d.z});
  }

  constexpr Vec3<T>& operator[](std::size_t c) { return cols[c]; }
  constexpr const Vec3<T>& operator[](std::size_t c) const { return cols[c]; }
  constexpr T& operator()(std::size_t r, std::size_t c) { return cols[c][r]; }
  constexpr const T& operator()(std::size_t r, std::size_t c) const { return cols[c][r]; }

  friend constexpr bool operator==(const Mat3&, const Mat3&) = default;
};

template <typename T>
struct Mat4 {
  std::array<Vec4<T>, 4> cols{Vec4<T>{1, 0, 0, 0}, Vec4<T>{0, 1, 0, 0}, Vec4<T>{0, 0, 1, 0},
                              Vec4<T>{0, 0, 0, 1}};

  static constexpr Mat4 identity() { return {}; }
  static constexpr Mat4 from_columns(const Vec4<T>& c0, const Vec4<T>& c1, const Vec4<T>& c2,
                                     const Vec4<T>& c3) {
    Mat4 m;
    m.cols = {c0, c1, c2, c3};
    return m;
  }

  constexpr Vec4<T>& operator[](std::size_t c) { return cols[c]; }
  constexpr const Vec4<T>& operator[](std::size_t c) const { return cols[c]; }
  constexpr T& operator()(std::size_t r, std::size_t c) { return cols[c][r]; }
  constexpr const T& operator()(std::size_t r, std::size_t c) const { return cols[c][r]; }

  friend constexpr bool operator==(const Mat4&, const Mat4&) = default;
};

using dmat3 = Mat3<double>;
using dmat4 = Mat4<double>;
using fmat3 = Mat3<float>;
using fmat4 = Mat4<float>;

// --- products -----------------------------------------------------------

template <typename T>
constexpr Vec3<T> operator*(const Mat3<T>& m, const Vec3<T>& v) {
  return m[0] * v.x + m[1] * v.y + m[2] * v.z;
}

template <typename T>
constexpr Vec4<T> operator*(const Mat4<T>& m, const Vec4<T>& v) {
  return m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3] * v.w;
}

template <typename T>
constexpr Mat3<T> operator*(const Mat3<T>& a, const Mat3<T>& b) {
  return Mat3<T>::from_columns(a * b[0], a * b[1], a * b[2]);
}

template <typename T>
constexpr Mat4<T> operator*(const Mat4<T>& a, const Mat4<T>& b) {
  return Mat4<T>::from_columns(a * b[0], a * b[1], a * b[2], a * b[3]);
}

template <typename T>
constexpr Mat3<T> operator*(const Mat3<T>& m, T s) {
  return Mat3<T>::from_columns(m[0] * s, m[1] * s, m[2] * s);
}

template <typename T>
constexpr Mat3<T> transpose(const Mat3<T>& m) {
  return Mat3<T>::from_columns({m(0, 0), m(0, 1), m(0, 2)}, {m(1, 0), m(1, 1), m(1, 2)},
                               {m(2, 0), m(2, 1), m(2, 2)});
}

template <typename T>
constexpr Mat4<T> transpose(const Mat4<T>& m) {
  Mat4<T> r;
  for (std::size_t c = 0; c < 4; ++c) {
    for (std::size_t row = 0; row < 4; ++row) {
      r(row, c) = m(c, row);
    }
  }
  return r;
}

template <typename T>
constexpr T determinant(const Mat3<T>& m) {
  return dot(m[0], cross(m[1], m[2]));
}

// Inverse by the adjugate. Returns nullopt when |det| <= epsilon.
template <typename T>
std::optional<Mat3<T>> inverse(const Mat3<T>& m, T epsilon = T{0}) {
  const T det = determinant(m);
  if (std::abs(det) <= epsilon) {
    return std::nullopt;
  }
  // Rows of the inverse are the cross products of pairs of columns.
  const Vec3<T> r0 = cross(m[1], m[2]);
  const Vec3<T> r1 = cross(m[2], m[0]);
  const Vec3<T> r2 = cross(m[0], m[1]);
  const T inv = T{1} / det;
  return transpose(Mat3<T>::from_columns(r0, r1, r2)) * inv;
}

// General 4x4 inverse by cofactor expansion. Returns nullopt when singular.
template <typename T>
std::optional<Mat4<T>> inverse(const Mat4<T>& m, T epsilon = T{0}) {
  // Element (r, c) as a flat row-major array for readability.
  std::array<T, 16> a{};
  for (std::size_t r = 0; r < 4; ++r) {
    for (std::size_t c = 0; c < 4; ++c) {
      a[r * 4 + c] = m(r, c);
    }
  }
  std::array<T, 16> inv{};
  inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
           a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
  inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
           a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
  inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
           a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
  inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
            a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
  inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
           a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
  inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
           a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
  inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
           a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
  inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
            a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
  inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] +
           a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
  inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
           a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
  inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
            a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
  inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
            a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
  inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
           a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
  inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] +
           a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
  inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] -
            a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
  inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] +
            a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];

  const T det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
  if (std::abs(det) <= epsilon) {
    return std::nullopt;
  }
  const T scale = T{1} / det;
  Mat4<T> out;
  for (std::size_t r = 0; r < 4; ++r) {
    for (std::size_t c = 0; c < 4; ++c) {
      out(r, c) = inv[r * 4 + c] * scale;
    }
  }
  return out;
}

// Element-wise comparison within an absolute tolerance.
template <typename T>
bool approx_equal(const Mat3<T>& a, const Mat3<T>& b, T tolerance) {
  for (std::size_t c = 0; c < 3; ++c) {
    if (!approx_equal(a[c], b[c], tolerance)) return false;
  }
  return true;
}

template <typename T>
bool approx_equal(const Mat4<T>& a, const Mat4<T>& b, T tolerance) {
  for (std::size_t c = 0; c < 4; ++c) {
    for (std::size_t r = 0; r < 4; ++r) {
      if (std::abs(a(r, c) - b(r, c)) > tolerance) return false;
    }
  }
  return true;
}

// --- explicit precision conversions -------------------------------------

inline fmat3 to_fp32(const dmat3& m) {
  return fmat3::from_columns(to_fp32(m[0]), to_fp32(m[1]), to_fp32(m[2]));
}
inline fmat4 to_fp32(const dmat4& m) {
  return fmat4::from_columns(to_fp32(m[0]), to_fp32(m[1]), to_fp32(m[2]), to_fp32(m[3]));
}
inline dmat3 to_fp64(const fmat3& m) {
  return dmat3::from_columns(to_fp64(m[0]), to_fp64(m[1]), to_fp64(m[2]));
}
inline dmat4 to_fp64(const fmat4& m) {
  return dmat4::from_columns(to_fp64(m[0]), to_fp64(m[1]), to_fp64(m[2]), to_fp64(m[3]));
}

// --- hashing (column by column) -----------------------------------------

template <typename T>
void hash_append(Hasher& h, const Mat3<T>& m) {
  for (const auto& c : m.cols) h.add(c);
}
template <typename T>
void hash_append(Hasher& h, const Mat4<T>& m) {
  for (const auto& c : m.cols) h.add(c);
}

}  // namespace vs::core

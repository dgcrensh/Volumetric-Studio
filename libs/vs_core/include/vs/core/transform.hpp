#pragma once

// Affine transforms: p' = linear * p + translation.
//
// Authored placements use dtransform (FP64). ftransform exists for display
// and generated data. A transform is kept as a 3x3 linear part plus a
// translation rather than a full 4x4, so it can never pick up a projective
// row by accident.

#include <cmath>
#include <optional>

#include "vs/core/hash.hpp"
#include "vs/core/mat.hpp"
#include "vs/core/vec.hpp"

namespace vs::core {

template <typename T>
struct Transform {
  Mat3<T> linear = Mat3<T>::identity();
  Vec3<T> translation{};

  static constexpr Transform identity() { return {}; }

  static constexpr Transform translate(const Vec3<T>& offset) {
    Transform t;
    t.translation = offset;
    return t;
  }

  static constexpr Transform scale(const Vec3<T>& factors) {
    Transform t;
    t.linear = Mat3<T>::diagonal(factors);
    return t;
  }

  static constexpr Transform scale(T factor) { return scale({factor, factor, factor}); }

  // Right-handed rotation by `radians` about `axis` (Rodrigues' formula).
  // The axis is normalised; a zero axis gives the identity.
  static Transform rotate(const Vec3<T>& axis, T radians) {
    const Vec3<T> a = normalize(axis);
    if (a == Vec3<T>{}) {
      return identity();
    }
    const T c = std::cos(radians);
    const T s = std::sin(radians);
    const T k = T{1} - c;
    Transform t;
    t.linear = Mat3<T>::from_columns(
        {c + a.x * a.x * k, a.y * a.x * k + a.z * s, a.z * a.x * k - a.y * s},
        {a.x * a.y * k - a.z * s, c + a.y * a.y * k, a.z * a.y * k + a.x * s},
        {a.x * a.z * k + a.y * s, a.y * a.z * k - a.x * s, c + a.z * a.z * k});
    return t;
  }

  [[nodiscard]] constexpr Vec3<T> apply_point(const Vec3<T>& p) const {
    return linear * p + translation;
  }

  [[nodiscard]] constexpr Vec3<T> apply_vector(const Vec3<T>& v) const { return linear * v; }

  // Normals transform by the inverse transpose; the result is normalised.
  // Returns nullopt for a singular transform.
  [[nodiscard]] std::optional<Vec3<T>> apply_normal(const Vec3<T>& n) const {
    const auto inv = inverse(linear);
    if (!inv) return std::nullopt;
    return normalize(transpose(*inv) * n);
  }

  [[nodiscard]] constexpr Mat4<T> to_matrix() const {
    return Mat4<T>::from_columns({linear[0], T{0}}, {linear[1], T{0}}, {linear[2], T{0}},
                                 {translation, T{1}});
  }

  friend constexpr bool operator==(const Transform&, const Transform&) = default;
};

using dtransform = Transform<double>;
using ftransform = Transform<float>;

// Composition: (a * b).apply_point(p) == a.apply_point(b.apply_point(p)).
template <typename T>
constexpr Transform<T> operator*(const Transform<T>& a, const Transform<T>& b) {
  Transform<T> r;
  r.linear = a.linear * b.linear;
  r.translation = a.linear * b.translation + a.translation;
  return r;
}

// Returns nullopt for a singular transform (|det| <= epsilon).
template <typename T>
std::optional<Transform<T>> inverse(const Transform<T>& t, T epsilon = T{0}) {
  const auto inv = inverse(t.linear, epsilon);
  if (!inv) return std::nullopt;
  Transform<T> r;
  r.linear = *inv;
  r.translation = -(*inv * t.translation);
  return r;
}

template <typename T>
bool approx_equal(const Transform<T>& a, const Transform<T>& b, T tolerance) {
  return approx_equal(a.linear, b.linear, tolerance) &&
         approx_equal(a.translation, b.translation, tolerance);
}

inline ftransform to_fp32(const dtransform& t) {
  ftransform r;
  r.linear = to_fp32(t.linear);
  r.translation = to_fp32(t.translation);
  return r;
}

inline dtransform to_fp64(const ftransform& t) {
  dtransform r;
  r.linear = to_fp64(t.linear);
  r.translation = to_fp64(t.translation);
  return r;
}

template <typename T>
void hash_append(Hasher& h, const Transform<T>& t) {
  h.add(t.linear).add(t.translation);
}

}  // namespace vs::core

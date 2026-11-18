#pragma once

// Axis-aligned boxes. A default-constructed box is empty (min > max), so
// expanding it by a point gives the box around that point alone.

#include <array>
#include <limits>

#include "vs/core/hash.hpp"
#include "vs/core/transform.hpp"
#include "vs/core/vec.hpp"

namespace vs::core {

template <typename T>
struct Box3 {
  Vec3<T> min{std::numeric_limits<T>::infinity(), std::numeric_limits<T>::infinity(),
              std::numeric_limits<T>::infinity()};
  Vec3<T> max{-std::numeric_limits<T>::infinity(), -std::numeric_limits<T>::infinity(),
              -std::numeric_limits<T>::infinity()};

  static constexpr Box3 empty() { return {}; }

  static constexpr Box3 from_min_max(const Vec3<T>& lo, const Vec3<T>& hi) {
    Box3 b;
    b.min = lo;
    b.max = hi;
    return b;
  }

  static constexpr Box3 from_center_half_extent(const Vec3<T>& center, const Vec3<T>& half) {
    return from_min_max(center - half, center + half);
  }

  [[nodiscard]] constexpr bool is_empty() const {
    return min.x > max.x || min.y > max.y || min.z > max.z;
  }

  [[nodiscard]] constexpr Vec3<T> center() const { return (min + max) * T{0.5}; }
  [[nodiscard]] constexpr Vec3<T> size() const {
    return is_empty() ? Vec3<T>{} : max - min;
  }
  [[nodiscard]] constexpr T volume() const {
    const Vec3<T> s = size();
    return s.x * s.y * s.z;
  }

  constexpr Box3& expand(const Vec3<T>& p) {
    min = vs::core::min(min, p);
    max = vs::core::max(max, p);
    return *this;
  }

  constexpr Box3& expand(const Box3& other) {
    if (!other.is_empty()) {
      expand(other.min);
      expand(other.max);
    }
    return *this;
  }

  // Grows the box by `margin` on every side.
  [[nodiscard]] constexpr Box3 inflated(T margin) const {
    if (is_empty()) return *this;
    const Vec3<T> m{margin, margin, margin};
    return from_min_max(min - m, max + m);
  }

  // Closed containment, widened by `tolerance` on every side.
  [[nodiscard]] constexpr bool contains(const Vec3<T>& p, T tolerance = T{0}) const {
    return p.x >= min.x - tolerance && p.x <= max.x + tolerance && p.y >= min.y - tolerance &&
           p.y <= max.y + tolerance && p.z >= min.z - tolerance && p.z <= max.z + tolerance;
  }

  [[nodiscard]] constexpr bool contains(const Box3& other, T tolerance = T{0}) const {
    return !other.is_empty() && contains(other.min, tolerance) && contains(other.max, tolerance);
  }

  // True when the closed boxes share any point, widened by `tolerance`.
  [[nodiscard]] constexpr bool intersects(const Box3& other, T tolerance = T{0}) const {
    if (is_empty() || other.is_empty()) return false;
    return min.x <= other.max.x + tolerance && other.min.x <= max.x + tolerance &&
           min.y <= other.max.y + tolerance && other.min.y <= max.y + tolerance &&
           min.z <= other.max.z + tolerance && other.min.z <= max.z + tolerance;
  }

  [[nodiscard]] constexpr std::array<Vec3<T>, 8> corners() const {
    return {Vec3<T>{min.x, min.y, min.z}, Vec3<T>{max.x, min.y, min.z},
            Vec3<T>{min.x, max.y, min.z}, Vec3<T>{max.x, max.y, min.z},
            Vec3<T>{min.x, min.y, max.z}, Vec3<T>{max.x, min.y, max.z},
            Vec3<T>{min.x, max.y, max.z}, Vec3<T>{max.x, max.y, max.z}};
  }

  // The axis-aligned box around the transformed corners.
  [[nodiscard]] Box3 transformed(const Transform<T>& t) const {
    if (is_empty()) return *this;
    Box3 out;
    for (const auto& c : corners()) out.expand(t.apply_point(c));
    return out;
  }

  friend constexpr bool operator==(const Box3&, const Box3&) = default;
};

using dbox3 = Box3<double>;
using fbox3 = Box3<float>;

// The overlap of two boxes; empty when they don't overlap.
template <typename T>
constexpr Box3<T> intersection(const Box3<T>& a, const Box3<T>& b) {
  if (a.is_empty() || b.is_empty()) return Box3<T>::empty();
  const Box3<T> r = Box3<T>::from_min_max(max(a.min, b.min), min(a.max, b.max));
  return r.is_empty() ? Box3<T>::empty() : r;
}

inline fbox3 to_fp32(const dbox3& b) {
  return b.is_empty() ? fbox3::empty() : fbox3::from_min_max(to_fp32(b.min), to_fp32(b.max));
}
inline dbox3 to_fp64(const fbox3& b) {
  return b.is_empty() ? dbox3::empty() : dbox3::from_min_max(to_fp64(b.min), to_fp64(b.max));
}

// Every empty box hashes alike, whatever its stored corners.
template <typename T>
void hash_append(Hasher& h, const Box3<T>& b) {
  h.add(b.is_empty());
  if (!b.is_empty()) h.add(b.min).add(b.max);
}

}  // namespace vs::core

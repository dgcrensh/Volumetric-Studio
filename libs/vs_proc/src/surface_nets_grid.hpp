#pragma once

// The grid's tetrahedra and padded samples, shared by surface nets
// (mesher.cpp) and its serial reference (surface_nets_reference.cpp).
// Private to vs_proc.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "vs/proc/field.hpp"

namespace vs::proc::nets {

using core::dvec3;

using Index = std::size_t;
using Vec3i = std::array<std::int64_t, 3>;

constexpr std::uint32_t kNone = 0xffffffffu;

inline bool inside(float v) { return v < 0.0f; }

inline double double_area(const dvec3& a, const dvec3& b, const dvec3& c) {
  return length(cross(b - a, c - a));
}

// --- the grid's tetrahedra ---------------------------------------------------------------
//
// Corner c of a cube sits at (c & 1, (c >> 1) & 1, (c >> 2) & 1). Each cube
// is split into six tetrahedra round its diagonal from corner 0 to corner 7
// (the Kuhn subdivision): tetrahedron t, for the axis order kOrder[t] =
// (a, b, c), has corners 0, e_a, e_a + e_b and 7. Every cube is split the
// same way, so neighbouring cubes' tetrahedra meet face to face, and the
// grid edges of the subdivision run along seven directions: the axes, the
// face diagonals (1, 1, 0) and so on, and the body diagonal (1, 1, 1).

constexpr std::array<std::array<Index, 3>, 6> kOrder = {
    {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};

constexpr std::array<Index, 4> tet_corners(Index t) {
  const Index a = Index{1} << kOrder[t][0];
  const Index b = Index{1} << kOrder[t][1];
  return {0, a, a | b, 7};
}

constexpr std::int64_t bit(Index corner, Index axis) {
  return static_cast<std::int64_t>((corner >> axis) & 1u);
}

constexpr std::array<Index, 7> kDirections = {1, 2, 4, 3, 6, 5, 7};

// The tetrahedra round the edge from a grid point P to P + direction: each
// as (offset of its cube's lowest corner from P, tetrahedron), in order
// counter-clockwise about the direction.
struct Round {
  std::vector<std::pair<Vec3i, Index>> tets;
};

inline std::array<Round, 7> make_rounds() {
  std::array<Round, 7> out;
  for (Index di = 0; di < 7; ++di) {
    const Index d = kDirections[di];
    const dvec3 dir = normalize(dvec3{static_cast<double>(bit(d, 0)), static_cast<double>(bit(d, 1)),
                                      static_cast<double>(bit(d, 2))});
    const dvec3 mid = dir * (0.5 * length(dvec3{static_cast<double>(bit(d, 0)),
                                                static_cast<double>(bit(d, 1)),
                                                static_cast<double>(bit(d, 2))}));
    const dvec3 any = std::abs(dir.x) < 0.9 ? dvec3{1, 0, 0} : dvec3{0, 1, 0};
    const dvec3 b1 = normalize(cross(cross(dir, any), dir));
    const dvec3 b2 = cross(dir, b1);
    std::vector<std::pair<double, std::pair<Vec3i, Index>>> found;
    for (Index o = 0; o < 8; ++o) {
      // Cube offset -1 or 0 on each axis; 0 where the edge runs along it.
      const Vec3i offset{-bit(o, 0), -bit(o, 1), -bit(o, 2)};
      bool ok = true;
      Index lu = 0;  // P as a corner of that cube
      for (Index axis = 0; axis < 3; ++axis) {
        if (bit(d, axis) != 0 && offset[axis] != 0) ok = false;
        if (offset[axis] != 0) lu |= Index{1} << axis;
      }
      if (!ok) continue;
      const Index lv = lu | d;  // P + direction (bits don't overlap: checked above)
      for (Index t = 0; t < 6; ++t) {
        const auto c = tet_corners(t);
        if (std::find(c.begin(), c.end(), lu) == c.end()) continue;
        if (std::find(c.begin(), c.end(), lv) == c.end()) continue;
        dvec3 centre{};
        for (const Index k : c) {
          centre = centre + dvec3{static_cast<double>(offset[0] + bit(k, 0)),
                                  static_cast<double>(offset[1] + bit(k, 1)),
                                  static_cast<double>(offset[2] + bit(k, 2))};
        }
        const dvec3 r = centre * 0.25 - mid;
        found.push_back({std::atan2(dot(r, b2), dot(r, b1)), {offset, t}});
      }
    }
    std::sort(found.begin(), found.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    for (const auto& f : found) out[di].tets.push_back(f.second);
  }
  return out;
}

inline const std::array<Round, 7>& rounds() {
  static const std::array<Round, 7> r = make_rounds();
  return r;
}

// The samples with a layer of "outside" all round, so the surface closes at
// the grid's edge. Padded index p is sample p - 1.
struct Padded {
  const SampledField& s;
  std::int64_t n[3];
  float outside;

  explicit Padded(const SampledField& samples)
      : s(samples),
        n{static_cast<std::int64_t>(samples.grid.nx) + 2,
          static_cast<std::int64_t>(samples.grid.ny) + 2,
          static_cast<std::int64_t>(samples.grid.nz) + 2},
        outside(static_cast<float>(samples.grid.cell)) {}

  [[nodiscard]] float at(std::int64_t i, std::int64_t j, std::int64_t k) const {
    if (i <= 0 || j <= 0 || k <= 0 || i >= n[0] - 1 || j >= n[1] - 1 || k >= n[2] - 1) {
      return outside;
    }
    return s.values[s.grid.index(static_cast<std::uint32_t>(i - 1), static_cast<std::uint32_t>(j - 1),
                                 static_cast<std::uint32_t>(k - 1))];
  }
  [[nodiscard]] dvec3 point(std::int64_t i, std::int64_t j, std::int64_t k) const {
    const GridSpec& g = s.grid;
    return {g.origin.x + static_cast<double>(i - 1) * g.cell,
            g.origin.y + static_cast<double>(j - 1) * g.cell,
            g.origin.z + static_cast<double>(k - 1) * g.cell};
  }
  [[nodiscard]] std::uint64_t cell_key(std::int64_t i, std::int64_t j, std::int64_t k) const {
    return (static_cast<std::uint64_t>(k) * static_cast<std::uint64_t>(n[1] - 1) +
            static_cast<std::uint64_t>(j)) *
               static_cast<std::uint64_t>(n[0] - 1) +
           static_cast<std::uint64_t>(i);
  }
};

}  // namespace vs::proc::nets

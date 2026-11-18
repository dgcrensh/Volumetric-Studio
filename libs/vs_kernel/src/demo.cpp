#include "vs/kernel/demo.hpp"

#include <numbers>
#include <vector>

#include "vs/kernel/operations.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/queries.hpp"

namespace vs::kernel {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kLength = 80, kWidth = 50, kThickness = 10;
constexpr double kCornerRadius = 5;
constexpr double kHoleRadius = 3, kHoleInset = 10;
constexpr double kBossRadius = 12, kBossHeight = 20;
constexpr double kBoreRadius = 6;

}  // namespace

double demo_part_volume() {
  const double plate = kLength * kWidth * kThickness -
                       4 * (1 - kPi / 4) * kCornerRadius * kCornerRadius * kThickness;
  const double boss = kPi * kBossRadius * kBossRadius * kBossHeight;
  const double bore = kPi * kBoreRadius * kBoreRadius * (kThickness + kBossHeight);
  const double holes = 4 * kPi * kHoleRadius * kHoleRadius * kThickness;
  return plate + boss - bore - holes;
}

Result<Solid> make_demo_part(const core::ProductSettings& settings) {
  // The plate is centred on the origin in x and y, sitting on z = 0.
  auto plate = make_box(core::dbox3::from_min_max({-kLength / 2, -kWidth / 2, 0},
                                                  {kLength / 2, kWidth / 2, kThickness}),
                        settings);
  if (!plate) return plate;

  // Round the four vertical edges: those parallel to z.
  std::vector<Edge> vertical;
  for (const Edge& e : edges(*plate)) {
    const auto box = bounding_box(e);
    if (box.size().z > kThickness / 2) vertical.push_back(e);
  }
  auto part = fillet(*plate, vertical, kCornerRadius);
  if (!part) return part;

  auto boss = make_cylinder({{0, 0, kThickness}, {0, 0, 1}}, kBossRadius, kBossHeight, settings);
  if (!boss) return boss;
  part = unite(*part, *boss);
  if (!part) return part;

  auto bore = make_cylinder({{0, 0, 0}, {0, 0, 1}}, kBoreRadius, kThickness + kBossHeight,
                            settings);
  if (!bore) return bore;
  part = cut(*part, *bore);
  if (!part) return part;

  for (const double sx : {-1.0, 1.0}) {
    for (const double sy : {-1.0, 1.0}) {
      const core::dvec3 c{sx * (kLength / 2 - kHoleInset), sy * (kWidth / 2 - kHoleInset), 0};
      auto hole = make_cylinder({c, {0, 0, 1}}, kHoleRadius, kThickness, settings);
      if (!hole) return hole;
      part = cut(*part, *hole);
      if (!part) return part;
    }
  }
  return part;
}

}  // namespace vs::kernel

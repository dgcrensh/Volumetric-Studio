#include "vs/kernel/tolerance_spike.hpp"

#include <cmath>
#include <functional>
#include <numbers>

#include <fmt/format.h>

#include "vs/kernel/operations.hpp"
#include "vs/kernel/primitives.hpp"
#include "vs/kernel/queries.hpp"

namespace vs::kernel {

using core::dvec3;

double SpikeCase::volume_relative_error() const {
  if (expected_volume == 0.0) return std::abs(volume);
  return std::abs(volume - expected_volume) / std::abs(expected_volume);
}

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kCell = 100.0;  // every scenario fits in a 100 mm cube

struct Scenario {
  std::string name;
  std::string description;
  // Builds the part in the cube [o, o + kCell]³ and returns it with the
  // analytic volume.
  std::function<Result<Solid>(const dvec3& o, const ProductSettings& s)> build;
  std::function<double(const ProductSettings& s)> expected_volume;
};

// Applies a chain of operations, stopping at the first failure.
Result<Solid> chain(Result<Solid> acc, const std::function<Result<Solid>(const Solid&)>& step) {
  if (!acc) return acc;
  return step(*acc);
}

Result<Solid> cell_box(const dvec3& o, const ProductSettings& s) {
  return make_box(core::dbox3::from_min_max(o, o + dvec3{kCell, kCell, kCell}), s);
}

std::vector<Scenario> scenarios() {
  std::vector<Scenario> out;

  out.push_back({"box", "A 100 mm cube",
                 [](const dvec3& o, const ProductSettings& s) { return cell_box(o, s); },
                 [](const ProductSettings&) { return kCell * kCell * kCell; }});

  out.push_back(
      {"drilled", "Cube with one 20 mm through hole (coplanar caps)",
       [](const dvec3& o, const ProductSettings& s) {
         return chain(cell_box(o, s), [&](const Solid& box) -> Result<Solid> {
           auto hole = make_cylinder({o + dvec3{50, 50, 0}, {0, 0, 1}}, 10, kCell, s);
           if (!hole) return hole;
           return cut(box, *hole);
         });
       },
       [](const ProductSettings&) { return kCell * kCell * kCell - kPi * 100 * kCell; }});

  out.push_back(
      {"drilled-25", "Cube drilled by 25 sequential 6 mm through holes",
       [](const dvec3& o, const ProductSettings& s) {
         Result<Solid> part = cell_box(o, s);
         for (int i = 0; i < 5 && part; ++i) {
           for (int j = 0; j < 5 && part; ++j) {
             part = chain(std::move(part), [&](const Solid& p) -> Result<Solid> {
               auto hole = make_cylinder({o + dvec3{20.0 + 15 * i, 20.0 + 15 * j, 0}, {0, 0, 1}},
                                         3, kCell, s);
               if (!hole) return hole;
               return cut(p, *hole);
             });
           }
         }
         return part;
       },
       [](const ProductSettings&) { return kCell * kCell * kCell - 25 * kPi * 9 * kCell; }});

  out.push_back(
      {"sphere-union", "Cube united with a 30 mm sphere centred on one of its corners",
       [](const dvec3& o, const ProductSettings& s) {
         return chain(cell_box(o, s), [&](const Solid& box) -> Result<Solid> {
           auto ball = make_sphere(o, 30, s);
           if (!ball) return ball;
           return unite(box, *ball);
         });
       },
       [](const ProductSettings&) {
         return kCell * kCell * kCell + 7.0 / 8.0 * 4.0 / 3.0 * kPi * 30 * 30 * 30;
       }});

  // Two perpendicular cylinders of radius r through the cube's centre:
  // their intersection is a Steinmetz solid of volume 16r³/3, and its edges
  // are approximated curves, the classic case for tolerance growth.
  const auto crossed = [](const dvec3& o, const ProductSettings& s, bool intersect) {
    auto a = make_cylinder({o + dvec3{0, 50, 50}, {1, 0, 0}}, 20, kCell, s);
    auto b = make_cylinder({o + dvec3{50, 0, 50}, {0, 1, 0}}, 20, kCell, s);
    if (!a) return a;
    if (!b) return b;
    return intersect ? common(*a, *b) : unite(*a, *b);
  };
  constexpr double kSteinmetz = 16.0 / 3.0 * 20 * 20 * 20;
  out.push_back({"crossed-cylinders-union", "Union of two perpendicular 20 mm cylinders",
                 [crossed](const dvec3& o, const ProductSettings& s) {
                   return crossed(o, s, false);
                 },
                 [](const ProductSettings&) { return 2 * kPi * 400 * kCell - kSteinmetz; }});
  out.push_back({"crossed-cylinders-common",
                 "Intersection of two perpendicular 20 mm cylinders (Steinmetz solid)",
                 [crossed](const dvec3& o, const ProductSettings& s) {
                   return crossed(o, s, true);
                 },
                 [](const ProductSettings&) { return kSteinmetz; }});

  out.push_back(
      {"min-feature-boss", "Cube with a boss of the minimum feature size on one face",
       [](const dvec3& o, const ProductSettings& s) {
         const double f = s.min_feature_size_mm();
         return chain(cell_box(o, s), [&](const Solid& box) -> Result<Solid> {
           auto boss = make_box(
               core::dbox3::from_min_max(o + dvec3{-f, 50, 50}, o + dvec3{0, 50 + f, 50 + f}), s);
           if (!boss) return boss;
           return unite(box, *boss);
         });
       },
       [](const ProductSettings& s) {
         const double f = s.min_feature_size_mm();
         return kCell * kCell * kCell + f * f * f;
       }});

  // Rolling-ball fillets on all twelve edges: blend surfaces meeting at the
  // corners, a classic source of tolerance growth.
  out.push_back(
      {"fillet-12", "Cube with all twelve edges rounded to 10 mm",
       [](const dvec3& o, const ProductSettings& s) {
         return chain(cell_box(o, s), [&](const Solid& box) -> Result<Solid> {
           const auto all = edges(box);
           return fillet(box, all, 10);
         });
       },
       [](const ProductSettings&) {
         const double r = 10;
         const double c = kCell - 2 * r;
         return c * c * c + 6 * c * c * r + 3 * kPi * r * r * c + 4.0 / 3.0 * kPi * r * r * r;
       }});

  // Two half-cubes separated by half the product tolerance. The spec counts
  // that as touching; the kernel decides for itself, unless the boolean is
  // given the product tolerance as its fuzzy distance.
  const auto near_coincident = [](const dvec3& o, const ProductSettings& s, double fuzzy) {
    const double gap = 0.5 * s.linear_tolerance_mm;
    auto a = make_box(core::dbox3::from_min_max(o, o + dvec3{50, kCell, kCell}), s);
    auto b = make_box(
        core::dbox3::from_min_max(o + dvec3{50 + gap, 0, 0}, o + dvec3{kCell, kCell, kCell}), s);
    if (!a) return a;
    if (!b) return b;
    return unite(*a, *b, BooleanOptions{fuzzy});
  };
  out.push_back(
      {"near-coincident", "Two half-cubes 0.5 × tolerance apart, united",
       [near_coincident](const dvec3& o, const ProductSettings& s) {
         return near_coincident(o, s, 0.0);
       },
       [](const ProductSettings& s) {
         return kCell * kCell * kCell - 0.5 * s.linear_tolerance_mm * kCell * kCell;
       }});
  out.push_back(
      {"near-coincident-fuzzy",
       "Two half-cubes 0.5 × tolerance apart, united with the tolerance as fuzzy distance",
       [near_coincident](const dvec3& o, const ProductSettings& s) {
         return near_coincident(o, s, s.linear_tolerance_mm);
       },
       [](const ProductSettings&) { return kCell * kCell * kCell; }});

  return out;
}

}  // namespace

std::vector<SpikeCase> run_tolerance_spike(const ProductSettings& settings) {
  const double e = settings.working_half_extent_mm;
  struct Placement {
    std::string_view name;
    dvec3 origin;
  };
  const Placement placements[] = {
      {"origin", {-kCell / 2, -kCell / 2, -kCell / 2}},
      {"corner", {e - kCell, e - kCell, e - kCell}},
  };

  std::vector<SpikeCase> out;
  for (const auto& scenario : scenarios()) {
    for (const auto& placement : placements) {
      SpikeCase c;
      c.name = fmt::format("{}@{}", scenario.name, placement.name);
      c.description = scenario.description;
      c.expected_volume = scenario.expected_volume(settings);
      auto part = scenario.build(placement.origin, settings);
      if (!part) {
        c.diagnostics.add(part.error());
        out.push_back(std::move(c));
        continue;
      }
      c.built = true;
      c.solids = part->solid_count();
      c.valid = check_validity(*part).empty();
      if (auto mass = mass_properties(*part)) c.volume = mass->volume;
      c.audit = audit_tolerances(*part, settings);
      out.push_back(std::move(c));
    }
  }
  return out;
}

}  // namespace vs::kernel

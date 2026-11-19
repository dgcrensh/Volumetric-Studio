#pragma once

// Primitive solids and planar profiles.
//
// All lengths are millimetres and angles radians (vs_core conventions). Every
// builder checks its inputs against the product settings: sizes must be
// finite and at least the minimum feature size, and the result must fit the
// working space. Failures come back as diagnostics, never exceptions.

#include <span>
#include <variant>
#include <vector>

#include "vs/core/box.hpp"
#include "vs/core/settings.hpp"
#include "vs/core/vec.hpp"
#include "vs/kernel/shape.hpp"

namespace vs::kernel {

using core::dbox3;
using core::dvec3;
using core::ProductSettings;

// A ray: an origin and a direction (normalised by the kernel).
struct Axis {
  dvec3 origin{};
  dvec3 direction{0, 0, 1};
};

// An infinite plane: a point on it and its unit normal. For a face of a
// solid, the normal points out of the material.
struct Plane {
  dvec3 origin{};
  dvec3 normal{0, 0, 1};
};

// --- profiles ---------------------------------------------------------------

struct LineSegment {
  dvec3 start{};
  dvec3 end{};
};

// A circular arc through three points, from `start` via `through` to `end`.
struct ArcSegment {
  dvec3 start{};
  dvec3 through{};
  dvec3 end{};
};

using ProfileSegment = std::variant<LineSegment, ArcSegment>;

// A closed loop of segments, in order: each segment starts where the previous
// one ends (within tolerance), and the last ends where the first starts.
struct Loop {
  std::vector<ProfileSegment> segments;

  // A closed polygon through `points` (at least three, not repeated at the end).
  static Loop polygon(std::span<const dvec3> points);
  // A full circle, as two half arcs. `normal` sets the circle's plane.
  static Loop circle(const dvec3& center, const dvec3& normal, double radius);
};

// A planar face bounded by `outer`, with `holes` cut out of it. Every loop
// must lie in one plane. Fails with "kernel.profile.*" codes for open loops,
// non-planar loops and holes that don't fit.
Result<Face> make_face(const Loop& outer, std::span<const Loop> holes,
                       const ProductSettings& settings);
inline Result<Face> make_face(const Loop& outer, const ProductSettings& settings) {
  return make_face(outer, {}, settings);
}

// --- primitive solids -----------------------------------------------------

// An axis-aligned box.
Result<Solid> make_box(const dbox3& box, const ProductSettings& settings);

// A cylinder whose base circle is centred on `axis.origin`, extending
// `height` along `axis.direction`.
Result<Solid> make_cylinder(const Axis& axis, double radius, double height,
                            const ProductSettings& settings);

Result<Solid> make_sphere(const dvec3& center, double radius, const ProductSettings& settings);

// Sweeps a planar face along `direction` (its length is the extrusion depth).
Result<Solid> make_prism(const Face& profile, const dvec3& direction,
                         const ProductSettings& settings);

// A prism whose side faces are drafted: they tilt by `draft_angle` radians
// about the profile's plane, so the section shrinks along `direction` for a
// positive angle and grows for a negative one (holes do the opposite, so the
// material always tapers the same way). |draft_angle| must be below 89°;
// zero gives make_prism(). Fails with "kernel.draft.failed" when the side
// faces can't be tilted that far (a section that shrinks to nothing).
Result<Solid> make_drafted_prism(const Face& profile, const dvec3& direction, double draft_angle,
                                 const ProductSettings& settings);

// Revolves a planar face by `angle` radians (0 < angle <= 2π) about `axis`.
// The axis must not cross the profile's interior.
Result<Solid> make_revolution(const Face& profile, const Axis& axis, double angle,
                              const ProductSettings& settings);

}  // namespace vs::kernel

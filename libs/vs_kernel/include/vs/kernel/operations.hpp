#pragma once

// Operations that make new solids from existing ones: booleans, transforms,
// edge fillets and chamfers, and shells.
//
// Every result is checked with the kernel's validity check before it is
// returned, so a caller never receives a solid the kernel itself considers
// broken. Failures carry stable codes:
//
// - kernel.boolean.failed          the boolean algorithm reported an error
// - kernel.boolean.invalid_result  it finished, but the result is not valid
// - kernel.boolean.empty_result    a union or cut left nothing (common may
//                                  be empty; that is a valid answer)
// - kernel.transform.singular      the transform cannot be inverted
// - kernel.fillet.failed           the fillet could not be built
// - kernel.chamfer.failed          the chamfer could not be built
// - kernel.shell.failed            the shell could not be built
//
// The fallback messages say what to try next (for example, a smaller fillet
// radius), as the spec's kernel table asks for OCCT's weak spots.

#include <span>

#include "vs/core/settings.hpp"
#include "vs/core/transform.hpp"
#include "vs/kernel/shape.hpp"

namespace vs::kernel {

struct BooleanOptions {
  // Distance within which the boolean treats faces, edges and vertices as
  // coincident. 0 uses the kernel's own tolerance (1e-7 mm), which leaves
  // gaps between that and the product tolerance as gaps; pass
  // ProductSettings::linear_tolerance_mm to close them (decision 0001).
  double fuzzy_mm = 0.0;
};

// a ∪ b
Result<Solid> unite(const Solid& a, const Solid& b, const BooleanOptions& options = {});
// a − b
Result<Solid> cut(const Solid& a, const Solid& b, const BooleanOptions& options = {});
// a ∩ b. An empty Solid (is_empty()) means the inputs don't overlap.
Result<Solid> common(const Solid& a, const Solid& b, const BooleanOptions& options = {});

// Where two faces meet: the region of `a` that also lies on `b`, as faces
// (a compound of them), within `options.fuzzy_mm` of each other. Empty when
// the faces only touch along an edge or at a point, or not at all. Used to
// find faces of neighbouring solids that lie against each other (M6).
Result<Shape> common_faces(const Face& a, const Face& b, const BooleanOptions& options = {});

// The same with several tools in one boolean: a ∪ tools[0] ∪ ... and
// a − tools[0] − .... The tools may overlap each other. One boolean with many
// tools is much faster than one boolean per tool (pattern copies). An empty
// span returns `a` unchanged.
Result<Solid> unite(const Solid& a, std::span<const Solid> tools,
                    const BooleanOptions& options = {});
Result<Solid> cut(const Solid& a, std::span<const Solid> tools,
                  const BooleanOptions& options = {});

// Applies an affine transform. Rigid motions and uniform scales keep the
// exact geometry; any other linear part converts surfaces to B-splines.
Result<Shape> transformed(const Shape& shape, const core::dtransform& transform);
Result<Solid> transformed(const Solid& solid, const core::dtransform& transform);

// Rounds `edges` of `solid` with a constant radius. Codes:
// kernel.fillet.invalid_radius, .foreign_edge, .failed.
Result<Solid> fillet(const Solid& solid, std::span<const Edge> edges, double radius);

// Bevels `edges` of `solid`, cutting back `distance` along both faces of
// each edge. Codes: kernel.chamfer.invalid_distance, .foreign_edge, .failed.
Result<Solid> chamfer(const Solid& solid, std::span<const Edge> edges, double distance);

// Hollows `solid` into walls `thickness` thick, leaving `open_faces` (at
// least one) open. The walls grow inwards from the outer surface, or
// outwards when `outward` is set (the original surface becomes the inside).
// A cylindrical or spherical face that would shrink by the thickness (convex
// when shelling inwards, concave when outwards) must have a larger radius,
// checked before the kernel runs. Codes: kernel.shell.invalid_thickness,
// .no_faces, .foreign_face, .too_thick, .failed.
Result<Solid> shell(const Solid& solid, std::span<const Face> open_faces, double thickness,
                    bool outward, const core::ProductSettings& settings);

}  // namespace vs::kernel

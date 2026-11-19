#pragma once

// Operations that make new solids from existing ones: booleans, transforms
// and edge fillets.
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
//
// The fallback messages say what to try next (for example, a smaller fillet
// radius), as the spec's kernel table asks for OCCT's weak spots.

#include <span>

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

// Applies an affine transform. Rigid motions and uniform scales keep the
// exact geometry; any other linear part converts surfaces to B-splines.
Result<Shape> transformed(const Shape& shape, const core::dtransform& transform);
Result<Solid> transformed(const Solid& solid, const core::dtransform& transform);

// Rounds `edges` of `solid` with a constant radius.
Result<Solid> fillet(const Solid& solid, std::span<const Edge> edges, double radius);

}  // namespace vs::kernel

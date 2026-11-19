#pragma once

// A sample part for `vstudio-cli kernel-demo` and the exchange tests: an
// 80 Ã— 50 Ã— 10 mm plate with rounded vertical corners, four 6 mm mounting
// holes, a 24 mm boss on top and a 12 mm bore through plate and boss. It
// exercises fillets, unions and cuts, and its volume is known analytically.

#include "vs/core/settings.hpp"
#include "vs/kernel/shape.hpp"

namespace vs::kernel {

Result<Solid> make_demo_part(const core::ProductSettings& settings);

// The demo part's exact volume in mmÂ³.
double demo_part_volume();

}  // namespace vs::kernel

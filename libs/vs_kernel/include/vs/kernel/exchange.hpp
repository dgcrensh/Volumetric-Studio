#pragma once

// STEP and IGES exchange.
//
// Files are written in millimetres. STEP output uses the AP242 schema.
// Reading returns everything the file holds as one shape (usually a compound);
// use `solids()` from queries.hpp or `Solid::from()` to get at the bodies.
// Failures carry "kernel.step.*" or "kernel.iges.*" codes and name the file.

#include <filesystem>

#include "vs/kernel/shape.hpp"

namespace vs::kernel {

Result<Shape> read_step(const std::filesystem::path& path);
// Returns an empty Diagnostics on success.
Diagnostics write_step(const Shape& shape, const std::filesystem::path& path);

Result<Shape> read_iges(const std::filesystem::path& path);
Diagnostics write_iges(const Shape& shape, const std::filesystem::path& path);

}  // namespace vs::kernel

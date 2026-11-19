#pragma once

// STEP and IGES exchange.
//
// Files are written in millimetres. STEP output uses the AP242 schema.
// Reading returns everything the file holds as one shape (usually a compound);
// use `solids()` from queries.hpp or `Solid::from()` to get at the bodies.
// Failures carry "kernel.step.*" or "kernel.iges.*" codes and name the file.

#include <filesystem>
#include <string>
#include <string_view>

#include "vs/core/hash.hpp"
#include "vs/kernel/shape.hpp"

namespace vs::kernel {

Result<Shape> read_step(const std::filesystem::path& path);
// Returns an empty Diagnostics on success.
Diagnostics write_step(const Shape& shape, const std::filesystem::path& path);

Result<Shape> read_iges(const std::filesystem::path& path);
Diagnostics write_iges(const Shape& shape, const std::filesystem::path& path);

// The kernel's own exact format (OCCT BRep text), for caches (M9): a lossless
// copy of the shape, written without triangulations, so a shape that has
// been tessellated writes the same text as one that hasn't. Not an exchange
// format: only this kernel reads it. An empty shape writes "" and "" reads
// back as an empty shape. Unlike STEP and IGES, these calls don't share
// global state and may run on several threads at once.
std::string write_brep(const Shape& shape);
Result<Shape> read_brep(std::string_view text);  // kernel.brep.read_failed

// BLAKE3 of write_brep(): the same for shapes built by the same operations
// from the same inputs, on any thread. Used to key and compare cached
// contents; it is not a geometric equivalence (the same box built two ways
// may hash differently).
core::Hash256 shape_hash(const Shape& shape);

}  // namespace vs::kernel

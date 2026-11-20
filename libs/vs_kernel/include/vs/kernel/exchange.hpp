#pragma once

// STEP and IGES exchange.
//
// Files are written in millimetres. STEP output uses the AP242 schema.
// Reading returns everything the file holds as one shape (usually a compound);
// use `solids()` from queries.hpp or `Solid::from()` to get at the bodies.
// Failures carry "kernel.step.*" or "kernel.iges.*" codes and name the file.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vs/core/hash.hpp"
#include "vs/core/vec.hpp"
#include "vs/kernel/shape.hpp"

namespace vs::kernel {

Result<Shape> read_step(const std::filesystem::path& path);
// Returns an empty Diagnostics on success.
Diagnostics write_step(const Shape& shape, const std::filesystem::path& path);

// --- Named parts (M17) ---------------------------------------------------------
//
// A design leaves the studio as one STEP file of named parts: an assembly
// called `assembly_name` with one component per part, each a product named
// after the part (B-rep solids, faces, or mesh shapes, below). Names are
// UTF-8. Reading returns the parts of the file's first assembly by product
// name, or every top-level shape when it has none.
struct NamedShape {
  std::string name;
  Shape shape;
};
Diagnostics write_step(std::span<const NamedShape> parts, const std::filesystem::path& path,
                       std::string_view assembly_name);
Result<std::vector<NamedShape>> read_step_parts(const std::filesystem::path& path);

// A shape that is only a triangle mesh: one face with no surface, carrying
// the triangles (counter-clockwise from outside) as its triangulation. STEP
// writes it as AP242 tessellated geometry (a triangulated surface set), so
// mesh bodies keep every triangle exactly as given, FP32 positions widened
// to FP64. Not a solid: booleans, fillets, checks and bounding_box() don't
// take it.
// kernel.mesh.invalid: no triangles, an index out of range, or a position
// that isn't finite.
Result<Shape> mesh_shape(std::span<const core::fvec3> positions,
                         std::span<const std::uint32_t> indices);
// Triangles stored on `shape`'s faces (by mesh_shape(), a STEP file's
// tessellated geometry or an earlier tessellation), without meshing.
std::size_t stored_triangle_count(const Shape& shape);

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

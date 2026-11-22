#pragma once

// Private: a field tree as one GLSL compute shader that samples it on a
// grid, operation for operation as vs_proc's FieldExpr::eval() does.
//
// The source depends only on the tree's shape (operations, how nodes share
// children, which leaves are meshes), never on its numbers: those go in
// the data buffers, so editing a parameter reuses the compiled program.
//
// Buffers (std430):
//   0  D: doubles. D[0..3] the grid's origin and cell; then each node's
//      numbers; then each mesh's positions, vertex normals, triangle
//      normals and BVH boxes.
//   1  U: uints. U[0] the node count N; U[1 + j] node j's offset in D,
//      U[1 + N + j] its count of numbers; from 1 + 2N, eight per mesh
//      (offsets of its arrays and flags); then triangle vertices, BVH
//      nodes and leaf order.
//   2  O: floats, one per sample of the slab dispatched.
// Uniforms: nx, ny (the grid), k0 (the slab's first z slice), count.

#include <cstdint>
#include <string>
#include <vector>

#include "vs/proc/field.hpp"

namespace vs::gpu {

struct FieldProgram {
  std::string source;
  std::vector<double> d;
  std::vector<std::uint32_t> u;
  std::size_t meshes = 0;  // distinct boundary meshes (MESH0 .. MESH{meshes - 1} in the source)
};

inline constexpr unsigned kLocalSize = 64;

FieldProgram translate(const proc::FieldExpr& root);

// GLSL for a double, exactly (its bits), never a decimal the driver parses.
std::string glsl_double(double x);

// The shared helpers (correctly rounded sqrt, std::min/max/clamp,
// sin_det/cos_det, ...), for tests that exercise them alone.
const std::string& prelude();

// The arithmetic alone (next_up, sqrt_rn, mn, mx, cl, dot3, len3, sub3,
// add3, mul3, nan_d, inf_d), for other shaders; they must `#define P
// precise` first.
const std::string& arithmetic_glsl();

}  // namespace vs::gpu

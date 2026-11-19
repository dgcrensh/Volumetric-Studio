#pragma once

// Mesh booleans through Manifold (private: Manifold's types stay in
// mesh_boolean.cpp, as the module rule asks).

#include <optional>
#include <string>

#include "vs/core/id.hpp"
#include "vs/mesh/mesh.hpp"

namespace vs::proc::detail {

enum class BooleanOp { Union, Difference, Intersection };

struct BooleanResult {
  std::optional<mesh::Mesh> mesh;  // set on success (may be empty: nothing left)
  int bad_input = 0;               // 1: `a` isn't a closed manifold, 2: `b`, 0: neither
  std::string message;             // why, when it failed
};

// a op b, every triangle of the result in `volume`. Both inputs must be
// closed, oriented 2-manifolds (Manifold's own check, after merging
// vertices that share a position).
BooleanResult mesh_boolean(const mesh::Mesh& a, const mesh::Mesh& b, BooleanOp op,
                           const core::Id& volume);

}  // namespace vs::proc::detail

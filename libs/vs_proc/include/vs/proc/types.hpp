#pragma once

// The procedural graph's type system (spec, "Data types per edge"; plan,
// M10).
//
// Every port has one PortType, and an edge joins ports of the same type
// only: there are no implicit conversions. Turning one type into another
// takes an explicit node, so its cost and loss of accuracy are visible in
// the graph (convert.field_to_mesh and convert.mesh_to_field, M11).
//
//   Scalar   one number (FP64: parameters and measurements are authored-grade)
//   Vector   three numbers (FP64), a point or a direction
//   Field    a signed distance or scalar field over space (field.hpp),
//            evaluated lazily, sampled to FP32
//   Mesh     a triangle mesh (vs_mesh, FP32)
//   Frames   a point set with orientations: placements (FP32 transforms)
//   Curves   polylines (FP32), for ribs and paths
//   List     a list of numbers
//
// A Value holds one of them. Values are immutable once made and cheap to
// copy (the big ones are shared pointers), so a node's outputs can be
// cached and handed to several consumers. hash_value() gives each value a
// content digest, which the per-node cache keys on (evaluate.hpp).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "vs/core/hash.hpp"
#include "vs/core/transform.hpp"
#include "vs/core/vec.hpp"
#include "vs/mesh/mesh.hpp"

namespace vs::proc {

using core::dvec3;
using core::fvec3;
using core::Hash256;
using core::Hasher;

enum class PortType { Scalar, Vector, Field, Mesh, Frames, Curves, List };

std::string_view to_string(PortType type);
std::optional<PortType> port_type_from_string(std::string_view text);

class FieldExpr;  // field.hpp
using Field = std::shared_ptr<const FieldExpr>;

using MeshValue = std::shared_ptr<const mesh::Mesh>;

struct FrameSet {
  std::vector<core::ftransform> frames;
};
using Frames = std::shared_ptr<const FrameSet>;

struct CurveSet {
  std::vector<std::vector<fvec3>> polylines;
};
using Curves = std::shared_ptr<const CurveSet>;

using List = std::vector<double>;

// In PortType order, so value.index() == static_cast<size_t>(type).
using Value = std::variant<double, dvec3, Field, MeshValue, Frames, Curves, List>;

[[nodiscard]] PortType type_of(const Value& value);

// A digest of the value's contents: equal values hash equal, whatever
// pointers they hold. Fields hash their expression tree (field.hpp).
[[nodiscard]] Hash256 hash_value(const Value& value);
void hash_append(Hasher& h, const Value& value);

// A short, readable rendering, for the CLI and messages.
[[nodiscard]] std::string describe(const Value& value);

}  // namespace vs::proc

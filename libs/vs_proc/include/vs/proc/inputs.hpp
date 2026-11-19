#pragma once

// What a procedural graph receives (spec, "Inputs"): the volume's boundary,
// the connection surfaces joining it with their types and parameters, its
// parameters, and its seed. Nothing else: no neighbours, no parent
// geometry, no layout features. The input nodes (nodes.hpp) are the only
// way a graph reads any of it, so this struct is the whole encapsulation
// contract on the procedural side.
//
// gather_inputs() builds it from a layout build (model::build_layout()),
// on the calling thread, because it tessellates the boundary and each
// patch with the kernel. Everything in it is plain data afterwards, safe
// to share across threads.
//
// Codes (the volume first):
//   model.contents.no_boundary   the boundary failed, so there's nothing to fill
//   proc.input.boundary          the boundary couldn't be tessellated into a
//                                closed mesh (mesh.* / kernel.* in the message)
//   proc.input.surface           a joining surface's patch failed or couldn't
//                                be tessellated (the surface second)

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vs/core/box.hpp"
#include "vs/core/diagnostic.hpp"
#include "vs/core/hash.hpp"
#include "vs/core/settings.hpp"
#include "vs/model/build.hpp"
#include "vs/model/document.hpp"
#include "vs/proc/distance.hpp"

namespace vs::proc {

using core::Hash256;
using core::Id;

struct ConnectionInput {
  Id id;
  std::string name;
  model::ConnectionType type = model::ConnectionType::Bonded;
  dvec3 origin{};       // the patch plane's frame
  dvec3 x_axis{1, 0, 0};
  dvec3 normal{0, 0, 1};
  double area = 0.0;    // mm²
  std::optional<double> gap;       // clearance
  std::optional<double> min_wall;  // bolted flange
  std::string thread;              // bolted flange, e.g. "M4"
  std::vector<dvec3> hole_centres;  // bolted flange
  std::shared_ptr<const TriangleDistance> patch;  // the patch, tessellated (open)
  Hash256 hash{};  // everything above
};

struct GraphInputs {
  Id volume;
  core::SubjectRef volume_subject;
  std::shared_ptr<const TriangleDistance> boundary;  // closed
  dbox3 bounds;                                      // the boundary's exact box
  std::vector<ConnectionInput> connections;          // document order
  std::map<std::string, double, std::less<>> parameters;  // the volume's, by name
  std::uint64_t seed = 0;
  core::ProductSettings settings;

  [[nodiscard]] const ConnectionInput* find_connection(const Id& surface) const;
};

// Tessellation of boundaries and patches for fields: fine enough that the
// tessellation's deflection is a small part of a typical grid cell.
inline constexpr double kInputDeflection = 0.01;  // mm

// The inputs of `document.volumes[index]` from `layout`.
core::Result<GraphInputs> gather_inputs(const model::Document& document,
                                        const model::DocumentBuild& layout, std::size_t index,
                                        const core::ProductSettings& settings);

}  // namespace vs::proc

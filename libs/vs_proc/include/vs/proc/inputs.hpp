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
//   proc.reference.boundary_face (V1) a face asked for isn't one of the
//                                boundary's named faces, or no face of the
//                                built boundary lies on it

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
  dvec3 origin{};       // the patch plane's frame; a curved one's axis (origin
  dvec3 x_axis{1, 0, 0};  // and normal) and its angle 0 (x_axis)
  dvec3 normal{0, 0, 1};
  // A curved (cylindrical, L2) surface's radius: its normal at a point is
  // radial, away from the axis, not `normal` (H3).
  std::optional<double> radius;
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
  // V1: the boundary's named faces that GatherOptions asked for, by name
  // ("<layout feature id>:<name>", or "-x".."+z" for a box), tessellated
  // (open, wound counter-clockwise seen from outside the boundary). Only
  // these: a graph names the faces it reads (boundary_faces_read()).
  std::map<std::string, std::shared_ptr<const TriangleDistance>, std::less<>> boundary_faces;
  std::map<std::string, double, std::less<>> parameters;  // the volume's, by name
  std::uint64_t seed = 0;
  core::ProductSettings settings;

  [[nodiscard]] const ConnectionInput* find_connection(const Id& surface) const;
};

// Tessellation of boundaries and patches for fields: fine enough that the
// tessellation's deflection is a small part of a typical grid cell.
inline constexpr double kInputDeflection = 0.01;  // mm

struct GatherOptions {
  // V1: named boundary faces to tessellate into GraphInputs::boundary_faces.
  // Each costs a tessellation of the boundary, so only those a graph reads.
  std::vector<std::string> boundary_faces;
};

// The inputs of `document.volumes[index]` from `layout`.
core::Result<GraphInputs> gather_inputs(const model::Document& document,
                                        const model::DocumentBuild& layout, std::size_t index,
                                        const core::ProductSettings& settings,
                                        const GatherOptions& options = {});

}  // namespace vs::proc

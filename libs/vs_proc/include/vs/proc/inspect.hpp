#pragma once

// Node output inspection (plan, M12: "any node's output can be evaluated
// and returned for preview"; the node editor, M15, draws it).
//
// inspect_node() runs one node and the nodes it depends on, nothing else,
// with the same per-node cache as generation (so inspecting a node the
// volume just generated through costs nothing), and returns every output
// of that node. The rest of the graph may be unfinished: no output node,
// other nodes' inputs unconnected. Its diagnostics are the evaluator's.
//
// preview() turns a value into something a viewport can draw:
//   Field          its region (where it is negative) meshed by surface nets
//                  on a grid over the boundary's box, `cell` apart (0: 32
//                  cells along the longest side, cheap), plus the samples
//   Mesh           the mesh itself
//   Frames         each frame's origin as a point (and the frames)
//   Curves         every polyline point
//   Scalar, Vector, List   text only
// Every preview has a short text (describe()). Preview meshes are not
// clipped to the boundary and know nothing of seams: they show the value,
// not the contents.

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/settings.hpp"
#include "vs/eval/thread_pool.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/model/build.hpp"
#include "vs/model/document.hpp"
#include "vs/proc/evaluate.hpp"
#include "vs/proc/field.hpp"
#include "vs/proc/graph.hpp"
#include "vs/proc/inputs.hpp"
#include "vs/proc/types.hpp"

namespace vs::proc {

struct NodeInspection {
  Id node;
  std::vector<std::pair<std::string, Value>> outputs;  // port name and value, in port order
  bool from_cache = false;  // the node itself came from the cache
  std::size_t evaluated = 0;  // nodes whose evaluate() ran
  core::Diagnostics diagnostics;

  [[nodiscard]] bool ok() const { return !outputs.empty() && !diagnostics.has_errors(); }
  [[nodiscard]] const Value* find(std::string_view port) const;
};

// `node` of `graph`, given the graph inputs. `options.only_for` and
// `keep_outputs` are set by the call.
NodeInspection inspect_node(const Graph& graph, const GraphInputs& inputs, const Id& node,
                            EvaluateGraphOptions options = {},
                            const NodeRegistry& registry = library());

// The same for `node` in the graph of `document.volumes[index]`, gathering
// its inputs from `layout` (on the calling thread: it tessellates).
NodeInspection inspect_volume_node(const model::Document& document,
                                   const model::DocumentBuild& layout, std::size_t index,
                                   const core::ProductSettings& settings, const Id& node,
                                   EvaluateGraphOptions options = {},
                                   const NodeRegistry& registry = library());

struct Preview {
  PortType type = PortType::Scalar;
  std::string text;
  std::optional<mesh::Mesh> mesh;      // Field (meshed region), Mesh
  std::optional<SampledField> samples;  // Field
  std::vector<core::fvec3> points;     // Frames origins, Curves points
};

// The preview grid's default: 32 cells along the boundary box's longest side.
inline constexpr double kPreviewCells = 32.0;

Preview preview(const Value& value, const GraphInputs& inputs, double cell = 0.0,
                eval::ThreadPool* pool = nullptr);

}  // namespace vs::proc

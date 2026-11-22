#include "vs/proc/inspect.hpp"

#include <algorithm>
#include <utility>

#include "vs/proc/mesher.hpp"

namespace vs::proc {

const Value* NodeInspection::find(std::string_view port) const {
  for (const auto& [name, value] : outputs) {
    if (name == port) return &value;
  }
  return nullptr;
}

NodeInspection inspect_node(const Graph& graph, const GraphInputs& inputs, const Id& node,
                            EvaluateGraphOptions options, const NodeRegistry& registry) {
  options.only_for = node;
  options.keep_outputs = true;
  GraphEvaluation ev = evaluate_graph(graph, inputs, options, registry);
  NodeInspection out;
  out.node = node;
  out.evaluated = ev.evaluated;
  out.diagnostics = std::move(ev.diagnostics);
  const NodeRun* run = ev.find(node);
  const Node* n = graph.find_node(node);
  const NodeType* type = n != nullptr ? registry.find(n->type) : nullptr;
  if (run == nullptr || type == nullptr || out.diagnostics.has_errors()) return out;
  out.from_cache = run->from_cache;
  for (std::size_t k = 0; k < type->outputs.size() && k < run->outputs.size(); ++k) {
    out.outputs.emplace_back(type->outputs[k].name, run->outputs[k]);
  }
  return out;
}

NodeInspection inspect_volume_node(const model::Document& document,
                                   const model::DocumentBuild& layout, std::size_t index,
                                   const core::ProductSettings& settings, const Id& node,
                                   EvaluateGraphOptions options, const NodeRegistry& registry) {
  NodeInspection out;
  out.node = node;
  const model::Volume& v = document.volumes.at(index);
  const auto* contents = std::get_if<model::ProceduralContents>(&v.contents);
  // L4: a composite's generator inspects the same way.
  const auto* composite = std::get_if<model::CompositeContents>(&v.contents);
  if (contents == nullptr && (composite == nullptr || !composite->generates())) {
    const core::SubjectRef self = document.volume_subject(v.id);
    out.diagnostics.add(core::Diagnostic::error(
        "proc.json.parse", "'" + self.label + "' isn't a procedural volume", {self}));
    return out;
  }
  auto graph = graph_from_json(contents != nullptr ? contents->graph : composite->generator);
  if (!graph) {
    out.diagnostics.add(graph.error());
    return out;
  }
  auto inputs = gather_inputs(document, layout, index, settings);
  if (!inputs) {
    out.diagnostics.add(inputs.error());
    return out;
  }
  return inspect_node(*graph, *inputs, node, options, registry);
}

Preview preview(const Value& value, const GraphInputs& inputs, double cell, eval::ThreadPool* pool,
                const eval::CancellationToken* cancel) {
  Preview p;
  p.type = type_of(value);
  p.text = describe(value);
  if (const auto* f = std::get_if<Field>(&value); f != nullptr && *f) {
    if (cell <= 0.0) {
      const dvec3 size = inputs.bounds.size();
      cell = std::max(size.x, std::max(size.y, size.z)) / kPreviewCells;
    }
    auto grid = grid_covering(inputs.bounds, cell);
    if (!grid) return p;
    bool stopped = false;
    SampledField samples = sample(**f, *grid, pool, cancel, &stopped);
    if (stopped || (cancel != nullptr && cancel->cancelled())) {
      p.cancelled = true;
      return p;
    }
    FieldMesh meshed = surface_nets(samples, pool, cancel);
    if (cancel != nullptr && cancel->cancelled()) {
      p.cancelled = true;
      return p;
    }
    p.mesh = to_mesh(meshed, inputs.volume);
    p.samples = std::move(samples);
  } else if (const auto* m = std::get_if<MeshValue>(&value); m != nullptr && *m) {
    p.mesh = **m;
  } else if (const auto* fr = std::get_if<Frames>(&value); fr != nullptr && *fr) {
    for (const auto& t : (*fr)->frames) p.points.push_back(t.translation);
  } else if (const auto* c = std::get_if<Curves>(&value); c != nullptr && *c) {
    for (const auto& line : (*c)->polylines) p.points.insert(p.points.end(), line.begin(), line.end());
  } else if (const auto* ch = std::get_if<Children>(&value); ch != nullptr && *ch) {
    // L4: each child's box, slightly shrunk so neighbours read apart.
    mesh::Mesh boxes;
    for (const auto& child : (*ch)->children) {
      const dvec3 inset = child.box.size() * 0.02;
      const core::fvec3 lo = core::to_fp32(child.box.min + inset);
      const core::fvec3 hi = core::to_fp32(child.box.max - inset);
      const auto base = static_cast<std::uint32_t>(boxes.positions.size());
      for (int k = 0; k < 8; ++k) {
        boxes.positions.push_back({(k & 1) != 0 ? hi.x : lo.x, (k & 2) != 0 ? hi.y : lo.y,
                               (k & 4) != 0 ? hi.z : lo.z});
        boxes.seam_vertices.push_back({});
      }
      // Counter-clockwise from outside.
      static constexpr std::uint32_t kFaces[12][3] = {
          {0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
          {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
      for (const auto& face : kFaces) {
        for (const std::uint32_t i : face) boxes.indices.push_back(base + i);
        boxes.triangle_volumes.push_back(inputs.volume);
      }
    }
    p.mesh = std::move(boxes);
  }
  return p;
}

}  // namespace vs::proc

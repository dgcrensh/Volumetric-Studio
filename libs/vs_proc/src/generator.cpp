#include "vs/proc/generator.hpp"

#include <utility>

#include <fmt/format.h>

#include "vs/mesh/seam_library.hpp"
#include "vs/proc/check.hpp"

namespace vs::proc {

using core::Diagnostic;
using core::Diagnostics;
using model::VolumeStatus;

Hash256 FieldContents::hash() const {
  Hasher h;
  h.add(std::string_view("vstudio.proc.field_contents"));
  h.add(field != nullptr);
  if (field) h.add(field->hash());
  h.add(samples.hash());
  h.add(static_cast<std::uint64_t>(stitched.size()));
  for (const auto& id : stitched) h.add(id);
  return h.finalize();
}

ProceduralGenerator::ProceduralGenerator(const NodeRegistry& registry) : registry_(registry) {}

namespace {

// Contents that failed before running anything.
eval::ContentsGenerator::Job failed(Diagnostics diagnostics) {
  return [d = std::move(diagnostics)](eval::GeneratedContents& out) {
    out.status = VolumeStatus::Failed;
    out.diagnostics = d;
  };
}

std::vector<model::OutputResult> unmeasured(const model::Volume& v) {
  std::vector<model::OutputResult> out;
  for (const auto& o : v.outputs) out.push_back({o.id, std::nullopt});
  return out;
}

}  // namespace

eval::ContentsGenerator::Job ProceduralGenerator::prepare(
    const model::Document& document, const model::DocumentBuild& layout, std::size_t index,
    const core::ProductSettings& settings, eval::ThreadPool& pool,
    const eval::CancellationToken& /*cancel*/) {
  const model::Volume& v = document.volumes.at(index);
  const core::SubjectRef self = document.volume_subject(v.id);
  const auto* contents = std::get_if<model::ProceduralContents>(&v.contents);
  if (contents == nullptr) {
    Diagnostics d;
    d.add(Diagnostic::error("proc.json.parse",
                            fmt::format("'{}' isn't a procedural volume", self.label), {self}));
    return failed(std::move(d));
  }

  auto parsed = graph_from_json(contents->graph);
  if (!parsed) {
    Diagnostic d = parsed.error();
    d.message = fmt::format("'{}': {}", self.label, d.message);
    d.subjects.push_back(self);
    Diagnostics ds;
    ds.add(std::move(d));
    return failed(std::move(ds));
  }
  auto graph = std::make_shared<const Graph>(std::move(parsed).value());

  {
    Diagnostics checked = check_volume_graph(*graph, document, v.id, registry_);
    if (checked.has_errors()) {
      Diagnostics ds;
      for (auto d : checked) {
        d.subjects.push_back(self);
        ds.add(std::move(d));
      }
      return failed(std::move(ds));
    }
  }

  auto gathered = gather_inputs(document, layout, index, settings);
  if (!gathered) {
    Diagnostics ds;
    ds.add(gathered.error());
    return failed(std::move(ds));
  }
  auto inputs = std::make_shared<const GraphInputs>(std::move(gathered).value());
  auto outputs = unmeasured(v);
  // The seams it must conform to; the library made the same ones, failures
  // included, so those are reported there, not here.
  auto seams = std::make_shared<const std::vector<mesh::Seam>>(
      mesh::build_seams(document, layout, settings, v.id).seams);

  return [this, graph, inputs, outputs, seams, &pool](eval::GeneratedContents& out) {
    EvaluateGraphOptions options;
    options.cache = &cache_;
    options.pool = &pool;
    GraphEvaluation ev = evaluate_graph(*graph, *inputs, options, registry_);
    evaluated_ += ev.evaluated;
    hits_ += ev.cache_hits;
    out.outputs = outputs;
    out.diagnostics = ev.diagnostics;
    if (!ev.ok()) {
      out.status = VolumeStatus::Failed;
      return;
    }
    auto grid = grid_covering(inputs->bounds, ev.cell_size);
    if (!grid) {
      Diagnostic d = grid.error();
      for (const auto& n : graph->nodes) {
        if (n.type == "output") d.subjects.push_back(node_subject(n));
      }
      d.subjects.push_back(inputs->volume_subject);
      out.diagnostics.add(std::move(d));
      out.status = VolumeStatus::Failed;
      return;
    }
    auto fc = std::make_shared<FieldContents>();
    fc->field = *ev.contents;
    fc->samples = sample(*fc->field, *grid, &pool);
    ContentsMesh cm =
        mesh_contents(fc->samples, *inputs->boundary, *seams, inputs->volume_subject, &pool);
    out.diagnostics.append(cm.diagnostics);
    fc->stitched = std::move(cm.stitched);
    fc->moved_vertices = cm.moved_vertices;
    out.extra = std::move(fc);
    if (!cm.ok()) {
      out.status = VolumeStatus::Failed;
      return;
    }
    out.mesh = std::move(cm.mesh);
    out.status = VolumeStatus::Current;
  };
}

std::shared_ptr<ProceduralGenerator> install(eval::Engine& engine) {
  auto g = std::make_shared<ProceduralGenerator>();
  engine.set_generator(model::VolumeKind::Procedural, g);
  return g;
}

const FieldContents* field_contents(const eval::VolumeResult& result) {
  if (!result.contents) return nullptr;
  return dynamic_cast<const FieldContents*>(result.contents->extra.get());
}

const mesh::Mesh* contents_mesh(const eval::VolumeResult& result) {
  if (!result.contents || !result.contents->mesh) return nullptr;
  return &*result.contents->mesh;
}

}  // namespace vs::proc

#pragma once

// Procedural volumes in the evaluation engine (plan, M10: "procedural
// volumes join the M9 scheduler").
//
// ProceduralGenerator is the eval::ContentsGenerator for
// model::VolumeKind::Procedural; install(engine) registers one. For each
// procedural volume the engine regenerates, it
//
//   1. (prepare, calling thread) parses the graph's JSON, checks it against
//      the document (check_volume_graph()), gathers its inputs
//      (gather_inputs(), which tessellates the boundary and patches) and
//      makes the seams of the surfaces joining the volume (M11);
//   2. (job, worker thread) evaluates the graph with the generator's node
//      cache, samples the contents field on the grid covering the
//      boundary's bounding box, and meshes it (mesher.hpp: clipped to the
//      boundary, surface nets, seams stitched in).
//
// The result is the mesh (GeneratedContents::mesh), which the engine
// checks for containment and the weld takes as it is, plus FieldContents
// (an eval::ExtraContents): the field and its samples. The output hash
// covers the mesh, the samples and the grid, which is what the engine's
// output hashes and determinism check compare. Published outputs are
// measured on the mesh (M12): volume, mass, area, centre of gravity and
// envelope as eval::measure_mesh() measures any mesh, and quantity
// Published from the graph's output.publish node of the output's name.
//
// Codes (the volume last unless said otherwise):
//   proc.json.*, proc.graph.*, proc.reference.*, proc.input.*, proc.node.*
//   proc.sample.*            the grid (output node first)
//   proc.mesh.*              the mesh (mesher.hpp)
//   proc.output.unpublished  (warning) a Published output the graph doesn't
//                            publish: it has no value (the volume first)
//
// The engine's cancellation starts no new volumes, and a finished job's
// contents are valid for their key. A job that is running watches the
// token too (M16): between graph nodes, sampling slices, and before and
// after meshing; when cancelled it stops and sets `cancelled`, so its
// volume stays stale and nothing is cached.

#include <atomic>
#include <cstddef>
#include <map>
#include <string>
#include <memory>
#include <vector>

#include "vs/eval/engine.hpp"
#include "vs/eval/generator.hpp"
#include "vs/proc/evaluate.hpp"
#include "vs/proc/field.hpp"
#include "vs/proc/mesher.hpp"

namespace vs::proc {

struct FieldContents final : eval::ExtraContents {
  Field field;
  SampledField samples;                // the contents field, not clipped
  std::vector<core::Id> stitched;      // surfaces whose seams the mesh conforms to
  std::size_t moved_vertices = 0;      // moved inward for containment (mesher.hpp)
  [[nodiscard]] Hash256 hash() const override;
};

// Also the layout generator (L4): a composite's generator graph runs here,
// with the same node cache, and its output.children is the layout.
class ProceduralGenerator final : public eval::ContentsGenerator, public eval::LayoutGenerator {
 public:
  explicit ProceduralGenerator(const NodeRegistry& registry = library());

  Job prepare(const model::Document& document, const model::DocumentBuild& layout,
              std::size_t index, const core::ProductSettings& settings, eval::ThreadPool& pool,
              const eval::CancellationToken& cancel) override;

  eval::LayoutOutcome generate(const model::Document& document,
                               const model::DocumentBuild& layout, std::size_t index,
                               const core::ProductSettings& settings, eval::ThreadPool& pool,
                               const eval::CancellationToken& cancel) override;

  [[nodiscard]] NodeCache& cache() { return cache_; }
  // Node evaluate() calls and cache hits across every job so far.
  [[nodiscard]] std::size_t nodes_evaluated() const { return evaluated_.load(); }
  [[nodiscard]] std::size_t node_cache_hits() const { return hits_.load(); }

 private:
  const NodeRegistry& registry_;
  NodeCache cache_;
  std::atomic<std::size_t> evaluated_{0};
  std::atomic<std::size_t> hits_{0};
};

// The volume's published outputs for its contents' mesh and the graph's
// published numbers (warnings for names the graph doesn't publish).
std::vector<model::OutputResult> measure(const model::Volume& volume, const core::SubjectRef& subject,
                                         const mesh::Mesh& mesh,
                                         const std::map<std::string, double, std::less<>>& published,
                                         eval::ThreadPool& pool, core::Diagnostics& diagnostics);

// Creates a generator and installs it on `engine` for procedural volumes and,
// as its layout generator, for composites' generated children (L4).
std::shared_ptr<ProceduralGenerator> install(eval::Engine& engine);

// The field contents a volume shows, if it has any.
const FieldContents* field_contents(const eval::VolumeResult& result);
// The mesh a volume shows (procedural or static), if it has one.
const mesh::Mesh* contents_mesh(const eval::VolumeResult& result);

}  // namespace vs::proc

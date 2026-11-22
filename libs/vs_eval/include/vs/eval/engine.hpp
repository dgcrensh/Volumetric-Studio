#pragma once

// The evaluation engine (spec, "Evaluation engine"; plan, M9): one
// scheduler for every kind of volume.
//
// Engine::evaluate() runs the spec's order of evaluation:
//
//   1. model::check() the document; errors stop everything (nothing is
//      built, every volume keeps what it showed before, as Stale).
//   2. Rebuild the layout, boundaries, connection surfaces and parameters
//      (model::build_layout()) on the calling thread, and validate the
//      layout (validate::validate_layout()).
//   3. Work out each volume's cache key (cache.hpp). A key the cache holds
//      is reused; the rest regenerate, independent volumes in parallel on
//      the engine's thread pool (model::build_contents(); static mesh
//      files are read here, since vs_model leaves them to vs_mesh).
//   4. Check each new result for containment against its boundary
//      (validate::check_containment(), on the calling thread, since kernel
//      booleans may touch their arguments), measure and publish its
//      outputs, and feed them through links to their targets.
//   5. Optionally weld everything into one mesh (mesh::combine()).
//
// Links. A published output feeding a layout variable or a volume
// parameter makes the target's volumes depend on the source volume (the
// dependency graph, model/dependency.hpp, has no loops). Volumes are
// therefore scheduled in waves: a volume runs once every volume upstream
// of it has finished, and before each wave the layout is rebuilt with the
// link values found so far (only when they changed). A target whose source
// has no value yet keeps its own expression, as in vs_model.
//
// Generated children (L4). When a composite has a generator, after step 1
// the engine builds the layout once and asks the layout generator
// (set_layout_generator(); vs_proc's install()) for each such composite's
// children, cached by the composite's key, and expands them into the
// evaluated document (model::expand()), which is check()ed again. From
// then on generated children are volumes like any other: they appear in
// Evaluation::document and ::volumes (after the authored volumes), are
// validated with their siblings, generate in waves and weld. A composite
// whose generator fails, whose expansion check() refuses, or whose
// generated children fail a layout check (eval.generated.layout) is
// Failed; with no layout generator installed it is Stale. Documents
// without a generator take exactly the M9 path.
//
// Configurations (L5). The document is checked as authored, then its
// active configuration is written in (model::configured()) and that is
// what every later step sees. Keys hash values, never which configuration
// gave them, so switching configurations regenerates only volumes whose
// inputs differ, and switching back finds the rest in the cache.
//
// Volume states (model::VolumeStatus), on each VolumeResult:
//
//   Current       its contents match its current inputs (generated now, or
//                 found in the cache).
//   Failed        its contents failed to generate, or weren't contained, or
//                 its boundary or a parameter failed. It keeps showing its
//                 last valid contents, marked stale (`stale_contents`), and
//                 blocks only the volumes downstream of it.
//   Stale         not regenerated in this evaluation: blocked by a failed
//                 volume upstream, cancelled, a preview, or contents of a
//                 kind with no generator (procedural graphs, unless vs_proc
//                 installed one: proc::install(), M10). It shows its last
//                 valid contents, if any.
//   Regenerating  only while an evaluation runs (states(), progress).
//
// Preview tier. preview() (or `generate = false`) does steps 1 to 3
// without generating: every volume has its boundary at once, cached
// contents where the key is known, and its last contents, stale,
// elsewhere.
//
// Cancellation. When the token is cancelled the engine starts nothing new;
// results that finished are kept (and cached: they are valid for their
// keys), and every volume not reached keeps its previous contents as
// Stale. A generator's job may also stop part way (vs_proc's do, M16);
// its volume is then Stale too, as if never started. The engine's state is then exactly what an evaluation of the
// finished volumes alone would leave, so the next evaluation carries on
// from it.
//
// Determinism. With `check_determinism`, every volume that regenerates is
// generated twice and the two output hashes compared; a difference is the
// error eval.nondeterministic. Output hashes never depend on the number of
// threads: each volume's generation runs on one thread, and reductions go
// through the deterministic pool (thread_pool.hpp).
//
// Codes (the volume first):
//   eval.blocked            (warning) not regenerated: a volume upstream failed
//   eval.nondeterministic   regenerating twice gave different contents
//   eval.cancelled          (info) the evaluation was cancelled
//   eval.generated.layout   (L4) a composite's generated children fail the
//                           layout checks (the checks' own diagnostics are
//                           on Evaluation::layout)
//   eval.generated.no_generator  (L4, warning) a composite has a generator
//                           but no layout generator is installed
//   model.contents.missing_blob, mesh.io.*   a static mesh file
//   validate.containment, validate.undecided  containment
//   everything model::check(), build_layout(), build_contents() and
//   validate_layout() report, passed through

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/hash.hpp"
#include "vs/core/settings.hpp"
#include "vs/eval/cache.hpp"
#include "vs/eval/cancel.hpp"
#include "vs/eval/generator.hpp"
#include "vs/eval/thread_pool.hpp"
#include "vs/mesh/combine.hpp"
#include "vs/model/build.hpp"
#include "vs/model/project.hpp"
#include "vs/validate/layout.hpp"

namespace vs::eval {

using core::Diagnostics;
using core::Id;
using core::ProductSettings;
using model::VolumeStatus;

struct EngineOptions {
  std::size_t threads = 0;  // the pool's size; 0: one per hardware thread
  // The pool's workers' scheduling priority (thread_pool.hpp); the app runs
  // them Low so generation never starves the UI (H7).
  WorkerPriority worker_priority = WorkerPriority::Normal;
  // Generate every regenerating volume twice and compare (debug).
  bool check_determinism = false;
  // Where to keep the on-disk cache; empty: memory only.
  std::filesystem::path disk_cache{};
  // The memory cache's budget in bytes (ContentsCache, H5).
  std::size_t memory_cache_bytes = ContentsCache::kDefaultMemoryBudget;
  // Keep the last few layouts (and the last check) and reuse them while what
  // they were built from is unchanged (decision 0029). Off builds and checks
  // everything every time: the same results, for tests and benchmarks.
  bool reuse_layouts = true;
};

struct EvaluateOptions {
  bool generate = true;  // false: the preview tier
  bool weld = false;     // also build the combined mesh
  mesh::CombineOptions combine;
  CancellationToken cancel;
  ProgressCallback progress;
};

struct VolumeResult {
  Id volume;
  VolumeStatus status = VolumeStatus::Stale;
  std::optional<Hash256> key;  // none when it couldn't be worked out
  // What the volume shows: its current contents, or its last valid ones
  // (then `stale_contents`). Null when it has never had any.
  ContentsPtr contents;
  bool stale_contents = false;
  bool regenerated = false;  // generated in this evaluation
  bool from_cache = false;   // found in the cache in this evaluation
  bool from_disk = false;    // ... on disk
  // This evaluation's diagnostics about the volume's contents (generation,
  // containment, eval.*). Layout-level ones are on Evaluation::build.
  Diagnostics diagnostics;
};

struct EvaluationStats {
  std::size_t regenerated = 0;  // volumes generated (twice each with check_determinism)
  std::size_t cache_hits = 0;   // memory or disk
  std::size_t disk_hits = 0;
  std::size_t blocked = 0;
  std::size_t waves = 0;
  std::size_t layout_builds = 0;     // model::build_layout() runs
  std::size_t layouts_reused = 0;    // layouts taken from the engine's last ones (decision 0029)
  std::size_t layouts_generated = 0;  // L4: generators run (not cached)
};

struct Evaluation {
  bool checked = false;    // model::check() passed
  bool cancelled = false;
  // The document as evaluated: its active configuration (L5) and links
  // applied to their targets, generated children (L4) expanded.
  model::Document document;
  // The configuration evaluated (L5) and its name; null and empty: the
  // authored values.
  Id configuration;
  std::string configuration_name;
  // The last layout build, with each volume's shown contents applied (its
  // status and diagnostics are the engine's), for vs_mesh and vs_validate.
  model::DocumentBuild build;
  validate::LayoutReport layout;
  std::vector<VolumeResult> volumes;  // in document order
  std::optional<mesh::CombinedMesh> combined;
  // check(), then the layout's build, the layout checks, then each
  // volume's contents, then the weld.
  Diagnostics diagnostics;
  EvaluationStats stats;

  [[nodiscard]] bool ok() const { return !diagnostics.has_errors(); }
  [[nodiscard]] const VolumeResult* find(const Id& volume) const;
  // BLAKE3 over every volume's id, status and output hash in document
  // order, and the combined mesh when there is one: the whole result, for
  // comparing runs.
  [[nodiscard]] Hash256 output_hash() const;
};

// Owns the pool, the cache and each volume's last valid contents. One
// evaluation at a time (calls are serialised); states() may be called from
// any thread meanwhile.
class Engine {
 public:
  explicit Engine(ProductSettings settings, EngineOptions options = {});
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  Evaluation evaluate(const model::Project& project, const EvaluateOptions& options = {});
  Evaluation preview(const model::Project& project);

  // Each volume's status as the engine sees it now (Regenerating while a
  // volume runs), by id.
  [[nodiscard]] std::map<Id, VolumeStatus> states() const;

  [[nodiscard]] ContentsCache& cache() { return cache_; }
  [[nodiscard]] ThreadPool& pool() { return pool_; }
  [[nodiscard]] const ProductSettings& settings() const { return settings_; }
  [[nodiscard]] const EngineOptions& options() const { return options_; }

  // Forgets every volume's last valid contents and the memory cache.
  void reset();

  // Generates contents of `kind` with `generator` (generator.hpp) from now
  // on; null removes it. Not while an evaluation runs.
  void set_generator(model::VolumeKind kind, std::shared_ptr<ContentsGenerator> generator);
  [[nodiscard]] std::shared_ptr<ContentsGenerator> generator(model::VolumeKind kind) const;

  // Makes composites' generated children with `generator` from now on (L4);
  // null removes it, and such composites are then Stale. Not while an
  // evaluation runs.
  void set_layout_generator(std::shared_ptr<LayoutGenerator> generator);
  [[nodiscard]] std::shared_ptr<LayoutGenerator> layout_generator() const;

 private:
  ProductSettings settings_;
  EngineOptions options_;
  ThreadPool pool_;
  ContentsCache cache_;
  std::mutex evaluate_mutex_;
  mutable std::mutex state_mutex_;
  std::map<Id, VolumeStatus> states_;
  std::map<Id, ContentsPtr> last_valid_;
  std::map<model::VolumeKind, std::shared_ptr<ContentsGenerator>> generators_;
  std::shared_ptr<LayoutGenerator> layout_generator_;
  // Generated layouts by their composite's key (L4).
  std::map<Hash256, std::shared_ptr<const LayoutOutcome>> layouts_;

  // The last few layouts built, by their layout_inputs() (decision 0029).
  struct LayoutEntry {
    model::Document inputs;
    model::Document shape;  // model::layout_shape() of the same document
    std::shared_ptr<const model::DocumentBuild> build;
    std::shared_ptr<const validate::LayoutReport> report;  // null until validated
    std::shared_ptr<const LayoutShapeHashes> hashes;
  };
  std::vector<LayoutEntry> layout_memo_;  // most recently used first
  // The last document check()ed and what it found (decision 0029).
  std::optional<std::pair<model::Document, Diagnostics>> last_check_;
  struct Layout {
    model::DocumentBuild build;
    std::shared_ptr<const validate::LayoutReport> report;
    std::shared_ptr<const LayoutShapeHashes> hashes;
  };
  // The layout of `document`, from the memo when it can, and its layout
  // checks when `validated`.
  Layout layout_of(const model::Document& document, bool validated, EvaluationStats& stats);
  std::optional<Hash256> key_of(const model::DocumentIndex& document,
                                const model::DocumentBuild& layout, std::size_t index,
                                const LayoutShapeHashes* hashes) const;
  // The layout checks' ParallelFor: chunks on the pool.
  validate::ParallelFor parallel_for();

  struct Expansion;
  Expansion expand_generated(const model::Document& document, const CancellationToken& cancel,
                             const ProgressCallback& progress, EvaluationStats& stats);
};

// A project's caches (M17, model::CacheFiles): the files of every volume's
// current contents in `evaluation` that are cacheable(), except parametric
// contents, whose rebuild result (named faces for the app's references and
// picking) lives in memory only, so they rebuild on load instead. Load them
// with `engine.cache().insert_files()` before the first evaluation: keys
// that still match are hits, the rest (stale) are never looked up.
model::CacheFiles project_cache_files(const Evaluation& evaluation);

// The determinism check on its own: evaluates `project` with fresh engines
// of each thread count (no cache shared) and returns their output hashes.
std::vector<Hash256> output_hashes_by_threads(const model::Project& project,
                                              const ProductSettings& settings,
                                              const std::vector<std::size_t>& thread_counts,
                                              bool weld = false);

}  // namespace vs::eval

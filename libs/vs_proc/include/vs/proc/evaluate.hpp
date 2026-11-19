#pragma once

// Evaluating a graph (plan, M10: "graph evaluation is per-node cached by
// input hash"; "identical across thread counts and runs").
//
// evaluate_graph() checks the graph (check.hpp), then runs its nodes once
// each in topological order (ties in node order) on the calling thread.
// Parallelism lives inside nodes and in sampling (field.hpp), which split
// work into fixed chunks, so nothing depends on the number of threads.
//
// Per-node cache. A node's key is a digest of the engine version, its
// type, its settings, its derived seed and the hash of every input value
// (types.hpp). With a NodeCache, a node whose key is cached isn't run:
// changing one parameter re-runs only the nodes downstream of it. Nodes
// reading the graph inputs (NodeType::cached false) always run; they are
// cheap, and their outputs' hashes carry the inputs into every key below.
//
// The contents are what reaches the output node's "contents" input; the
// output node's `cell_size` property is the sampling grid's cell (0: auto,
// see grid_covering()).
//
// Every diagnostic gets the volume (inputs.volume_subject) appended as its
// last subject, so routing (vs_validate) can find the volume a node is in.

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/hash.hpp"
#include "vs/eval/cancel.hpp"
#include "vs/eval/thread_pool.hpp"
#include "vs/proc/graph.hpp"
#include "vs/proc/inputs.hpp"
#include "vs/proc/nodes.hpp"
#include "vs/proc/types.hpp"

namespace vs::proc {

// Node outputs by key. Thread-safe; shared by evaluations of any graphs
// (keys include everything a node's outputs depend on). When it holds
// more than `capacity` entries it forgets them all and starts again.
class NodeCache {
 public:
  explicit NodeCache(std::size_t capacity = 4096) : capacity_(capacity) {}
  [[nodiscard]] std::optional<std::vector<Value>> find(const Hash256& key) const;
  void insert(const Hash256& key, std::vector<Value> outputs);
  void clear();
  [[nodiscard]] std::size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::map<Hash256, std::vector<Value>> entries_;
  std::size_t capacity_;
};

struct EvaluateGraphOptions {
  NodeCache* cache = nullptr;
  eval::ThreadPool* pool = nullptr;
  const eval::CancellationToken* cancel = nullptr;
  // Keep every node's outputs on the result (for inspection and tests);
  // otherwise only the contents are kept.
  bool keep_outputs = false;
};

struct NodeRun {
  Id node;
  Hash256 key{};
  bool ran = false;        // evaluate() was called
  bool from_cache = false;
  std::vector<Value> outputs;  // with keep_outputs, in port order
};

struct GraphEvaluation {
  std::optional<Field> contents;  // set when the graph evaluated
  double cell_size = 0.0;         // the output node's
  std::vector<NodeRun> nodes;     // those reached, in evaluation order
  std::size_t evaluated = 0;      // nodes whose evaluate() ran
  std::size_t cache_hits = 0;
  bool cancelled = false;
  core::Diagnostics diagnostics;

  [[nodiscard]] bool ok() const { return contents.has_value() && !diagnostics.has_errors(); }
  [[nodiscard]] const NodeRun* find(const Id& node) const;
};

// The seed node `node` gets in a volume seeded `volume_seed`.
std::uint64_t derive_seed(std::uint64_t volume_seed, const Id& node);

GraphEvaluation evaluate_graph(const Graph& graph, const GraphInputs& inputs,
                               const EvaluateGraphOptions& options = {},
                               const NodeRegistry& registry = library());

// A graph parameter's value for these inputs: the volume's parameter of
// that name, else the graph's default.
std::optional<double> parameter_value(const Graph& graph, const GraphInputs& inputs,
                                      std::string_view name);

}  // namespace vs::proc

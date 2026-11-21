#pragma once

// Evaluating a graph (plan, M10: "graph evaluation is per-node cached by
// input hash"; "identical across thread counts and runs").
//
// evaluate_graph() checks the graph (check.hpp), then runs its nodes once
// each in topological order (ties in node order) on the calling thread.
// Parallelism lives inside nodes and in sampling (field.hpp), which split
// work into fixed chunks, so nothing depends on the number of threads.
//
// Per-node cache. A node's key is a digest of the engine version, the
// ProductSettings (inputs.settings, H3), its type, its settings, its
// derived seed, the volume's id and bounding box
// (M12: mesh nodes put the volume on their triangles, and
// convert.field_to_mesh grids the box) and the hash of every input value
// (types.hpp). With a NodeCache, a node whose key is cached isn't run:
// changing one parameter re-runs only the nodes downstream of it. Nodes
// reading the graph inputs (NodeType::cached false) always run; they are
// cheap, and their outputs' hashes carry the inputs into every key below.
//
// The contents are what reaches the output node's "contents" input; the
// output node's `cell_size` property is the sampling grid's cell (0: auto,
// see grid_covering()).
//
// Published numbers (M12): each output.publish node's value, by its name,
// for the volume's published outputs of quantity Published.
//
// Inspection (M12): with `only_for`, only that node and the nodes it
// depends on run, and the graph needn't be finished elsewhere (no output
// node, other inputs unconnected): what inspect_node() (inspect.hpp) uses.
//
// Every diagnostic gets the volume (inputs.volume_subject) appended as its
// last subject, so routing (vs_validate) can find the volume a node is in.

#include <cstddef>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
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
// (keys include everything a node's outputs depend on).
//
// Bounded by bytes (H5): entries count value_bytes() of their outputs, and
// once they add up to more than the budget the least recently found or
// inserted ones are forgotten (the newest always stays, however big).
class NodeCache {
 public:
  inline static constexpr std::size_t kDefaultBudget = std::size_t{1} << 30;  // 1 GiB

  explicit NodeCache(std::size_t byte_budget = kDefaultBudget) : budget_(byte_budget) {}
  [[nodiscard]] std::optional<std::vector<Value>> find(const Hash256& key) const;
  void insert(const Hash256& key, std::vector<Value> outputs);
  void clear();
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t bytes() const;
  [[nodiscard]] std::size_t budget() const { return budget_; }

 private:
  struct Entry {
    std::vector<Value> outputs;
    std::size_t bytes = 0;
    std::list<Hash256>::iterator use;
  };

  mutable std::mutex mutex_;
  std::map<Hash256, Entry> entries_;
  mutable std::list<Hash256> uses_;  // most recently used first
  std::size_t bytes_ = 0;
  std::size_t budget_;
};

struct EvaluateGraphOptions {
  NodeCache* cache = nullptr;
  eval::ThreadPool* pool = nullptr;
  const eval::CancellationToken* cancel = nullptr;
  // Keep every node's outputs on the result (for inspection and tests);
  // otherwise only the contents are kept.
  bool keep_outputs = false;
  // Run only this node and those it depends on (inspection).
  std::optional<Id> only_for;
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
  std::optional<Children> children;  // L4: a composite generator's output.children
  double cell_size = 0.0;         // the output node's
  std::map<std::string, double, std::less<>> published;  // output.publish values by name
  std::vector<NodeRun> nodes;     // those reached, in evaluation order
  std::size_t evaluated = 0;      // nodes whose evaluate() ran
  std::size_t cache_hits = 0;
  bool cancelled = false;
  core::Diagnostics diagnostics;

  [[nodiscard]] bool ok() const {
    return (contents.has_value() || children.has_value()) && !diagnostics.has_errors();
  }
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

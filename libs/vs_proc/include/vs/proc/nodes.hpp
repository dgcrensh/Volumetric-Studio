#pragma once

// The node API and the node library (spec, "Initial node families"; plan,
// M10).
//
// A NodeType declares its ports and properties and how it evaluates:
//
//   - inputs: typed ports. A Scalar or Vector input may be left
//     unconnected; it then takes the node's setting of the same name, else
//     the port's default. Inputs of other types must be connected.
//   - outputs: typed ports, all set by every successful evaluation.
//   - properties: configuration that isn't a port (a number, a vector, a
//     text choice, a connection surface's id or a graph parameter's name),
//     held in the node's settings.
//   - evaluate(NodeContext&): reads inputs, properties, the graph inputs
//     and its derived seed, and sets outputs. It must be deterministic: the
//     same inputs and seed always give the same outputs. A node's seed is
//     derived from the volume's seed and the node's id, so adding a node
//     never changes another node's randomness.
//
// When the inputs can't all be met (a negative radius, a division by zero,
// a minimum above a maximum) a node calls infeasible() naming the inputs
// that conflict; for anything else it calls fail(). Either way it reports
// one diagnostic and sets no outputs, and evaluation stops there (the nodes
// downstream of it can't run). Subjects: the node, then each input named
// (SubjectKind::Node, id derived from the node's id and the port, label
// "<node>.<port>"), then the volume (evaluate.hpp adds it).
//   proc.node.infeasible   these inputs can't all be met
//   proc.node.failed       the node failed (the message says why)
//
// Families in library(), by type name:
//   input.boundary, input.surface, input.parameter           graph inputs
//   query.distance_to_boundary, query.distance_to_surface    queries
//   field.constant, field.sphere, field.box, field.cylinder,
//   field.plane, field.half_space                            field primitives
//   field.union, field.intersection, field.difference, and
//   field.smooth_union, field.smooth_intersection,
//   field.smooth_difference                                  field booleans
//   field.offset, field.shell, field.negate, field.abs,
//   field.add, field.multiply, field.scale                   field maths
//   math.add, .subtract, .multiply, .divide, .min, .max, .abs, .negate,
//   .sqrt, .clamp, .random                                   scalar maths
//   logic.compare, logic.and, logic.or, logic.not, logic.select
//   vector.compose, .decompose, .add, .scale, .length
//   list.range, list.sum, list.count, list.get
//   output                                                   the contents
//
// Input and query nodes read the volume's inputs; they are cheap and never
// cached by node (their outputs' hashes still key everything downstream).

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/eval/thread_pool.hpp"
#include "vs/proc/graph.hpp"
#include "vs/proc/inputs.hpp"
#include "vs/proc/types.hpp"

namespace vs::proc {

struct PortSpec {
  std::string name;
  PortType type = PortType::Scalar;
  std::optional<Setting> default_value;  // Scalar and Vector inputs only
  std::string doc;
};

enum class PropertyKind {
  Number,
  Vector,
  Choice,     // one of `choices`
  Surface,    // a connection surface's id (32 hex digits) joining the volume
  Parameter,  // the name of one of the graph's parameters
};

struct PropertySpec {
  std::string name;
  PropertyKind kind = PropertyKind::Number;
  std::optional<Setting> default_value;  // none: the node must set it
  std::vector<std::string> choices;      // Choice
  std::string doc;
};

class NodeContext;

struct NodeType {
  std::string type;
  std::string category;
  std::string doc;
  std::vector<PortSpec> inputs;
  std::vector<PortSpec> outputs;
  std::vector<PropertySpec> properties;
  bool cached = true;  // false for nodes reading the graph inputs
  std::function<void(NodeContext&)> evaluate;

  [[nodiscard]] const PortSpec* find_input(std::string_view name) const;
  [[nodiscard]] const PortSpec* find_output(std::string_view name) const;
  [[nodiscard]] const PropertySpec* find_property(std::string_view name) const;
};

class NodeRegistry {
 public:
  void add(NodeType type);
  [[nodiscard]] const NodeType* find(std::string_view type) const;
  [[nodiscard]] std::vector<const NodeType*> types() const;  // by name

 private:
  std::map<std::string, NodeType, std::less<>> types_;
};

// The built-in node library.
const NodeRegistry& library();

// What a node's evaluate() sees.
class NodeContext {
 public:
  NodeContext(const Node& node, const NodeType& type, const Graph& graph,
              const GraphInputs& inputs, std::vector<Value> input_values, std::uint64_t seed,
              eval::ThreadPool* pool);

  [[nodiscard]] const Node& node() const { return node_; }
  [[nodiscard]] const Graph& graph() const { return graph_; }
  [[nodiscard]] const GraphInputs& inputs() const { return inputs_; }
  [[nodiscard]] std::uint64_t seed() const { return seed_; }
  [[nodiscard]] eval::ThreadPool* pool() const { return pool_; }
  [[nodiscard]] const core::ProductSettings& settings() const { return inputs_.settings; }

  // Inputs by port name (the type must match the port's; checked graphs
  // guarantee it).
  [[nodiscard]] const Value& input(std::string_view port) const;
  [[nodiscard]] double scalar(std::string_view port) const;
  [[nodiscard]] dvec3 vector(std::string_view port) const;
  [[nodiscard]] const Field& field(std::string_view port) const;
  [[nodiscard]] const List& list(std::string_view port) const;

  // Properties, from the node's settings or the declared default.
  [[nodiscard]] double number(std::string_view property) const;
  [[nodiscard]] dvec3 vector_property(std::string_view property) const;
  [[nodiscard]] std::string text(std::string_view property) const;

  void set(std::string_view port, Value value);

  // Reports that `ports` (inputs) can't all be met.
  void infeasible(std::vector<std::string> ports, std::string message);
  void fail(std::string message);

  // After evaluate(): the outputs in the type's port order, or the problem.
  [[nodiscard]] bool failed() const { return !diagnostics_.empty(); }
  [[nodiscard]] core::Diagnostics& diagnostics() { return diagnostics_; }
  [[nodiscard]] std::vector<std::optional<Value>>& outputs() { return outputs_; }

 private:
  const Node& node_;
  const NodeType& type_;
  const Graph& graph_;
  const GraphInputs& inputs_;
  std::vector<Value> input_values_;  // in the type's input order
  std::uint64_t seed_;
  eval::ThreadPool* pool_;
  std::vector<std::optional<Value>> outputs_;
  core::Diagnostics diagnostics_;
};

// A uniform double in [0, 1) from a seed and an index (splitmix64): the
// one source of randomness nodes use.
double uniform(std::uint64_t seed, std::uint64_t index);

}  // namespace vs::proc

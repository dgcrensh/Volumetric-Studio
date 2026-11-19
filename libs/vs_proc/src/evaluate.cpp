#include "vs/proc/evaluate.hpp"

#include <exception>
#include <utility>

#include <fmt/format.h>

#include "vs/core/version.hpp"
#include "vs/proc/check.hpp"
#include "vs/proc/field.hpp"

namespace vs::proc {

using core::Diagnostic;
using core::Diagnostics;

// --- cache ---------------------------------------------------------------------------

std::optional<std::vector<Value>> NodeCache::find(const Hash256& key) const {
  const std::lock_guard lock(mutex_);
  const auto it = entries_.find(key);
  if (it == entries_.end()) return std::nullopt;
  return it->second;
}

void NodeCache::insert(const Hash256& key, std::vector<Value> outputs) {
  const std::lock_guard lock(mutex_);
  if (entries_.size() >= capacity_) entries_.clear();
  entries_.insert_or_assign(key, std::move(outputs));
}

void NodeCache::clear() {
  const std::lock_guard lock(mutex_);
  entries_.clear();
}

std::size_t NodeCache::size() const {
  const std::lock_guard lock(mutex_);
  return entries_.size();
}

const NodeRun* GraphEvaluation::find(const Id& node) const {
  for (const auto& n : nodes) {
    if (n.node == node) return &n;
  }
  return nullptr;
}

std::uint64_t derive_seed(std::uint64_t volume_seed, const Id& node) {
  return core::hash_of(std::string_view("vstudio.proc.seed"), volume_seed, node).word(0);
}

std::optional<double> parameter_value(const Graph& graph, const GraphInputs& inputs,
                                      std::string_view name) {
  const auto it = inputs.parameters.find(name);
  if (it != inputs.parameters.end()) return it->second;
  if (const GraphParameter* p = graph.find_parameter(name)) return p->value;
  return std::nullopt;
}

namespace {

void hash_setting(Hasher& h, const Setting& s) {
  h.add(static_cast<std::uint64_t>(s.index()));
  std::visit([&](const auto& v) { h.add(v); }, s);
}

Value literal(const Setting& s) {
  if (const auto* d = std::get_if<double>(&s)) return *d;
  return std::get<core::dvec3>(s);
}

// Adds the volume as the last subject, so routing finds it.
void add_all(Diagnostics& out, const Diagnostics& in, const core::SubjectRef& volume) {
  for (auto d : in) {
    d.subjects.push_back(volume);
    out.add(std::move(d));
  }
}

}  // namespace

GraphEvaluation evaluate_graph(const Graph& graph, const GraphInputs& inputs,
                               const EvaluateGraphOptions& options, const NodeRegistry& registry) {
  GraphEvaluation ev;
  {
    Diagnostics d = check_graph(graph, registry);
    // Surfaces must be this volume's (check_volume_graph() says so at
    // authoring time; this catches a graph run against other inputs).
    for (const auto& node : graph.nodes) {
      const NodeType* type = registry.find(node.type);
      if (type == nullptr) continue;
      for (const auto& prop : type->properties) {
        if (prop.kind != PropertyKind::Surface) continue;
        const Setting* s = node.find_setting(prop.name);
        const auto* text = s != nullptr ? std::get_if<std::string>(s) : nullptr;
        const auto id = text != nullptr ? Id::parse(*text) : std::nullopt;
        if (id && inputs.find_connection(*id) == nullptr) {
          d.add(Diagnostic::error(
              "proc.reference.surface",
              fmt::format("'{}' reads connection surface {}, which doesn't join this volume",
                          node_subject(node).label, id->to_string()),
              {node_subject(node)}));
        }
      }
    }
    if (d.has_errors()) {
      add_all(ev.diagnostics, d, inputs.volume_subject);
      return ev;
    }
  }

  const std::vector<std::size_t> order = topological_order(graph);
  std::map<Id, std::vector<Value>> values;  // each node's outputs, in port order

  for (const std::size_t index : order) {
    if (options.cancel != nullptr && options.cancel->cancelled()) {
      ev.cancelled = true;
      return ev;
    }
    const Node& node = graph.nodes[index];
    const NodeType& type = *registry.find(node.type);

    // Inputs: an edge's value, else the setting, else the default.
    std::vector<Value> in;
    in.reserve(type.inputs.size());
    for (const auto& port : type.inputs) {
      if (const Edge* e = graph.edge_into(node.id, port.name)) {
        const NodeType& from = *registry.find(graph.find_node(e->from.node)->type);
        std::size_t k = 0;
        while (from.outputs[k].name != e->from.port) ++k;
        in.push_back(values.at(e->from.node).at(k));
      } else if (const Setting* s = node.find_setting(port.name)) {
        in.push_back(literal(*s));
      } else {
        in.push_back(literal(*port.default_value));
      }
    }

    const std::uint64_t seed = derive_seed(inputs.seed, node.id);
    Hasher h;
    h.add(std::string_view("vstudio.proc.node"));
    h.add(core::kEngineVersion);
    h.add(node.type);
    h.add(static_cast<std::uint64_t>(node.settings.size()));
    for (const auto& [k, v] : node.settings) {
      h.add(k);
      hash_setting(h, v);
    }
    h.add(seed);
    h.add(static_cast<std::uint64_t>(in.size()));
    for (const auto& v : in) h.add(v);

    NodeRun run;
    run.node = node.id;
    run.key = h.finalize();

    std::vector<Value> outputs;
    std::optional<std::vector<Value>> hit;
    if (type.cached && options.cache != nullptr) hit = options.cache->find(run.key);
    if (hit) {
      outputs = std::move(*hit);
      run.from_cache = true;
      ++ev.cache_hits;
    } else {
      NodeContext ctx(node, type, graph, inputs, in, seed, options.pool);
      try {
        type.evaluate(ctx);
      } catch (const std::exception& e) {
        ctx.fail(e.what());
      }
      run.ran = true;
      ++ev.evaluated;
      if (!ctx.failed()) {
        for (std::size_t k = 0; k < type.outputs.size(); ++k) {
          if (!ctx.outputs()[k]) {
            ctx.fail(fmt::format("it didn't produce its output '{}'", type.outputs[k].name));
            break;
          }
        }
      }
      if (ctx.failed()) {
        add_all(ev.diagnostics, ctx.diagnostics(), inputs.volume_subject);
        ev.nodes.push_back(std::move(run));
        return ev;
      }
      for (auto& o : ctx.outputs()) outputs.push_back(std::move(*o));
      if (type.cached && options.cache != nullptr) options.cache->insert(run.key, outputs);
    }

    if (node.type == "output") {
      ev.contents = std::get<Field>(in.at(0));
      const Setting* cell = node.find_setting("cell_size");
      ev.cell_size = cell != nullptr ? std::get<double>(*cell) : 0.0;
    }
    if (options.keep_outputs) run.outputs = outputs;
    values.emplace(node.id, std::move(outputs));
    ev.nodes.push_back(std::move(run));
  }
  return ev;
}

}  // namespace vs::proc

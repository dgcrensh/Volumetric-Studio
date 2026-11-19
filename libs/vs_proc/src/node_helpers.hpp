#pragma once

// Helpers the node library's sources share (private to vs_proc).

#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "vs/proc/nodes.hpp"

namespace vs::proc::detail {

using P = PortType;

inline PortSpec in(std::string name, PortType type, std::optional<Setting> def = std::nullopt,
            std::string doc = {}) {
  return {std::move(name), type, std::move(def), std::move(doc)};
}
inline PortSpec out(std::string name, PortType type, std::string doc = {}) {
  return {std::move(name), type, std::nullopt, std::move(doc)};
}
inline Setting num(double v) { return v; }
inline Setting vec(double x, double y, double z) { return dvec3{x, y, z}; }

// A length that must be at least the minimum feature size.
inline bool check_size(NodeContext& ctx, std::string_view port, double v) {
  const double min = ctx.settings().min_feature_size_mm();
  if (!std::isfinite(v) || v < min) {
    ctx.infeasible({std::string(port)},
                   fmt::format("{} is {} mm; it must be at least {} mm", port, v, min));
    return false;
  }
  return true;
}

inline bool check_direction(NodeContext& ctx, std::string_view port, const dvec3& v) {
  if (!(length(v) > 0.0) || !std::isfinite(length(v))) {
    ctx.infeasible({std::string(port)}, fmt::format("{} must be a non-zero direction", port));
    return false;
  }
  return true;
}

inline NodeType make(std::string type, std::string category, std::string doc, std::vector<PortSpec> inputs,
              std::vector<PortSpec> outputs, std::function<void(NodeContext&)> evaluate,
              std::vector<PropertySpec> properties = {}, bool cached = true) {
  NodeType t;
  t.type = std::move(type);
  t.category = std::move(category);
  t.doc = std::move(doc);
  t.inputs = std::move(inputs);
  t.outputs = std::move(outputs);
  t.properties = std::move(properties);
  t.cached = cached;
  t.evaluate = std::move(evaluate);
  return t;
}

inline const ConnectionInput* surface_of(NodeContext& ctx) {
  const auto id = Id::parse(ctx.text("surface"));
  const ConnectionInput* c = id ? ctx.inputs().find_connection(*id) : nullptr;
  if (c == nullptr) ctx.fail("its connection surface doesn't join this volume");
  return c;
}

// An optional input of a non-literal type.
inline PortSpec optional_in(std::string name, PortType type, std::string doc = {}) {
  PortSpec p{std::move(name), type, std::nullopt, std::move(doc)};
  p.optional = true;
  return p;
}

// A whole number in [lo, hi], or infeasible.
inline bool check_count(NodeContext& ctx, std::string_view port, double v, double lo, double hi) {
  if (!(v >= lo && v <= hi) || v != std::floor(v)) {
    ctx.infeasible({std::string(port)},
                   fmt::format("{} is {}; it must be a whole number from {} to {}", port, v, lo, hi));
    return false;
  }
  return true;
}

// Registration of the M12 families (nodes_infill.cpp, nodes_frames.cpp,
// nodes_mesh.cpp).
void add_infill_nodes(NodeRegistry& r);
void add_frame_nodes(NodeRegistry& r);
void add_mesh_nodes(NodeRegistry& r);

}  // namespace vs::proc::detail

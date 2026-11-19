#include "vs/proc/nodes.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>

#include "vs/proc/field.hpp"

namespace vs::proc {

using core::Diagnostic;
using core::SubjectKind;
using core::SubjectRef;

// --- types and registry --------------------------------------------------------------

const PortSpec* NodeType::find_input(std::string_view name) const {
  for (const auto& p : inputs) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

const PortSpec* NodeType::find_output(std::string_view name) const {
  for (const auto& p : outputs) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

const PropertySpec* NodeType::find_property(std::string_view name) const {
  for (const auto& p : properties) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

void NodeRegistry::add(NodeType type) {
  std::string name = type.type;
  types_.insert_or_assign(std::move(name), std::move(type));
}

const NodeType* NodeRegistry::find(std::string_view type) const {
  const auto it = types_.find(type);
  return it == types_.end() ? nullptr : &it->second;
}

std::vector<const NodeType*> NodeRegistry::types() const {
  std::vector<const NodeType*> out;
  for (const auto& [_, t] : types_) out.push_back(&t);
  return out;
}

// --- context -------------------------------------------------------------------------

NodeContext::NodeContext(const Node& node, const NodeType& type, const Graph& graph,
                         const GraphInputs& inputs, std::vector<Value> input_values,
                         std::uint64_t seed, eval::ThreadPool* pool)
    : node_(node),
      type_(type),
      graph_(graph),
      inputs_(inputs),
      input_values_(std::move(input_values)),
      seed_(seed),
      pool_(pool),
      outputs_(type.outputs.size()) {}

const Value& NodeContext::input(std::string_view port) const {
  for (std::size_t i = 0; i < type_.inputs.size(); ++i) {
    if (type_.inputs[i].name == port) return input_values_.at(i);
  }
  throw std::logic_error(fmt::format("node type {} has no input {}", type_.type, port));
}

double NodeContext::scalar(std::string_view port) const { return std::get<double>(input(port)); }
dvec3 NodeContext::vector(std::string_view port) const { return std::get<dvec3>(input(port)); }
const Field& NodeContext::field(std::string_view port) const {
  return std::get<Field>(input(port));
}
const List& NodeContext::list(std::string_view port) const { return std::get<List>(input(port)); }

namespace {

const Setting& property_setting(const Node& node, const NodeType& type, std::string_view name) {
  if (const Setting* s = node.find_setting(name)) return *s;
  const PropertySpec* spec = type.find_property(name);
  if (spec == nullptr || !spec->default_value) {
    throw std::logic_error(fmt::format("node type {} has no property {}", type.type, name));
  }
  return *spec->default_value;
}

}  // namespace

double NodeContext::number(std::string_view property) const {
  return std::get<double>(property_setting(node_, type_, property));
}
dvec3 NodeContext::vector_property(std::string_view property) const {
  return std::get<dvec3>(property_setting(node_, type_, property));
}
std::string NodeContext::text(std::string_view property) const {
  return std::get<std::string>(property_setting(node_, type_, property));
}

void NodeContext::set(std::string_view port, Value value) {
  for (std::size_t i = 0; i < type_.outputs.size(); ++i) {
    if (type_.outputs[i].name == port) {
      outputs_[i] = std::move(value);
      return;
    }
  }
  throw std::logic_error(fmt::format("node type {} has no output {}", type_.type, port));
}

void NodeContext::infeasible(std::vector<std::string> ports, std::string message) {
  const SubjectRef self = node_subject(node_);
  std::vector<SubjectRef> subjects{self};
  std::string names;
  for (const auto& p : ports) {
    subjects.push_back({SubjectKind::Node, Id::derive(node_.id, p), fmt::format("{}.{}", self.label, p)});
    if (!names.empty()) names += ", ";
    names += p;
  }
  diagnostics_.add(Diagnostic::error(
      "proc.node.infeasible",
      fmt::format("'{}': these inputs can't all be met ({}): {}", self.label, names, message),
      std::move(subjects)));
}

void NodeContext::fail(std::string message) {
  const SubjectRef self = node_subject(node_);
  diagnostics_.add(Diagnostic::error("proc.node.failed",
                                     fmt::format("'{}' failed: {}", self.label, message), {self}));
}

double uniform(std::uint64_t seed, std::uint64_t index) {
  std::uint64_t z = seed + (index + 1) * 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z = z ^ (z >> 31);
  return static_cast<double>(z >> 11) * 0x1.0p-53;
}

// --- the library ---------------------------------------------------------------------

namespace {

using P = PortType;

PortSpec in(std::string name, PortType type, std::optional<Setting> def = std::nullopt,
            std::string doc = {}) {
  return {std::move(name), type, std::move(def), std::move(doc)};
}
PortSpec out(std::string name, PortType type, std::string doc = {}) {
  return {std::move(name), type, std::nullopt, std::move(doc)};
}
Setting num(double v) { return v; }
Setting vec(double x, double y, double z) { return dvec3{x, y, z}; }

// A length that must be at least the minimum feature size.
bool check_size(NodeContext& ctx, std::string_view port, double v) {
  const double min = ctx.settings().min_feature_size_mm();
  if (!std::isfinite(v) || v < min) {
    ctx.infeasible({std::string(port)},
                   fmt::format("{} is {} mm; it must be at least {} mm", port, v, min));
    return false;
  }
  return true;
}

bool check_direction(NodeContext& ctx, std::string_view port, const dvec3& v) {
  if (!(length(v) > 0.0) || !std::isfinite(length(v))) {
    ctx.infeasible({std::string(port)}, fmt::format("{} must be a non-zero direction", port));
    return false;
  }
  return true;
}

NodeType make(std::string type, std::string category, std::string doc, std::vector<PortSpec> inputs,
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

const ConnectionInput* surface_of(NodeContext& ctx) {
  const auto id = Id::parse(ctx.text("surface"));
  const ConnectionInput* c = id ? ctx.inputs().find_connection(*id) : nullptr;
  if (c == nullptr) ctx.fail("its connection surface doesn't join this volume");
  return c;
}

void add_inputs(NodeRegistry& r) {
  r.add(make(
      "input.boundary", "input", "The volume's boundary as a signed distance field (negative inside)",
      {}, {out("field", P::Field), out("min", P::Vector), out("max", P::Vector), out("size", P::Vector)},
      [](NodeContext& ctx) {
        const GraphInputs& gi = ctx.inputs();
        ctx.set("field", mesh_signed_field(gi.boundary));
        ctx.set("min", gi.bounds.min);
        ctx.set("max", gi.bounds.max);
        ctx.set("size", gi.bounds.size());
      },
      {}, false));

  r.add(make(
      "input.surface", "input",
      "A connection surface joining the volume: its plane, frame, type parameters and bolt holes",
      {},
      {out("plane", P::Field, "signed distance to the surface's plane, positive along its normal"),
       out("origin", P::Vector), out("normal", P::Vector), out("area", P::Scalar),
       out("gap", P::Scalar, "clearance gap (0 otherwise)"),
       out("min_wall", P::Scalar, "bolted flange minimum wall (0 otherwise)"),
       out("holes", P::Frames, "a frame at each bolt hole, z along the normal")},
      [](NodeContext& ctx) {
        const ConnectionInput* c = surface_of(ctx);
        if (c == nullptr) return;
        ctx.set("plane", half_space_field(c->origin, c->normal));
        ctx.set("origin", c->origin);
        ctx.set("normal", c->normal);
        ctx.set("area", c->area);
        ctx.set("gap", c->gap.value_or(0.0));
        ctx.set("min_wall", c->min_wall.value_or(0.0));
        auto frames = std::make_shared<FrameSet>();
        const dvec3 z = normalize(c->normal);
        const dvec3 x = normalize(c->x_axis);
        const dvec3 y = cross(z, x);
        for (const auto& h : c->hole_centres) {
          core::ftransform t;
          t.linear.cols = {core::to_fp32(x), core::to_fp32(y), core::to_fp32(z)};
          t.translation = core::to_fp32(h);
          frames->frames.push_back(t);
        }
        ctx.set("holes", Frames(std::move(frames)));
      },
      {{"surface", PropertyKind::Surface, std::nullopt, {}, "the surface's id"}}, false));

  r.add(make(
      "input.parameter", "input", "One of the graph's parameters (the volume's value, else the default)",
      {}, {out("value", P::Scalar)},
      [](NodeContext& ctx) {
        const std::string name = ctx.text("name");
        const auto it = ctx.inputs().parameters.find(name);
        if (it != ctx.inputs().parameters.end()) {
          ctx.set("value", it->second);
          return;
        }
        const GraphParameter* p = ctx.graph().find_parameter(name);
        if (p == nullptr) {
          ctx.fail(fmt::format("the graph has no parameter '{}'", name));
          return;
        }
        ctx.set("value", p->value);
      },
      {{"name", PropertyKind::Parameter, std::nullopt, {}, "the parameter's name"}}, false));
}

void add_queries(NodeRegistry& r) {
  r.add(make(
      "query.distance_to_boundary", "query", "Distance to the boundary's surface (unsigned)", {},
      {out("distance", P::Field)},
      [](NodeContext& ctx) { ctx.set("distance", mesh_unsigned_field(ctx.inputs().boundary)); }, {},
      false));
  r.add(make(
      "query.distance_to_surface", "query", "Distance to a connection surface's patch (unsigned)", {},
      {out("distance", P::Field)},
      [](NodeContext& ctx) {
        const ConnectionInput* c = surface_of(ctx);
        if (c == nullptr) return;
        ctx.set("distance", mesh_unsigned_field(c->patch));
      },
      {{"surface", PropertyKind::Surface, std::nullopt, {}, "the surface's id"}}, false));
}

void add_field_primitives(NodeRegistry& r) {
  r.add(make("field.constant", "field", "The same value everywhere", {in("value", P::Scalar, num(0))},
             {out("field", P::Field)},
             [](NodeContext& ctx) { ctx.set("field", constant_field(ctx.scalar("value"))); }));
  r.add(make("field.sphere", "field", "A sphere",
             {in("centre", P::Vector, vec(0, 0, 0)), in("radius", P::Scalar, num(10))},
             {out("field", P::Field)}, [](NodeContext& ctx) {
               const double radius = ctx.scalar("radius");
               if (!check_size(ctx, "radius", radius)) return;
               ctx.set("field", sphere_field(ctx.vector("centre"), radius));
             }));
  r.add(make("field.box", "field", "An axis-aligned box",
             {in("centre", P::Vector, vec(0, 0, 0)), in("size", P::Vector, vec(10, 10, 10))},
             {out("field", P::Field)}, [](NodeContext& ctx) {
               const dvec3 size = ctx.vector("size");
               const double min = ctx.settings().min_feature_size_mm();
               if (!(size.x >= min && size.y >= min && size.z >= min) || !std::isfinite(length(size))) {
                 ctx.infeasible({"size"}, fmt::format("every side must be at least {} mm", min));
                 return;
               }
               ctx.set("field", box_field(ctx.vector("centre"), size));
             }));
  r.add(make("field.cylinder", "field", "A capped cylinder, centred on `centre` along `axis`",
             {in("centre", P::Vector, vec(0, 0, 0)), in("axis", P::Vector, vec(0, 0, 1)),
              in("radius", P::Scalar, num(5)), in("height", P::Scalar, num(10))},
             {out("field", P::Field)}, [](NodeContext& ctx) {
               const double radius = ctx.scalar("radius");
               const double height = ctx.scalar("height");
               if (!check_direction(ctx, "axis", ctx.vector("axis"))) return;
               if (!check_size(ctx, "radius", radius) || !check_size(ctx, "height", height)) return;
               ctx.set("field", cylinder_field(ctx.vector("centre"), ctx.vector("axis"), radius, height));
             }));
  r.add(make("field.plane", "field", "Distance to a plane (unsigned)",
             {in("point", P::Vector, vec(0, 0, 0)), in("normal", P::Vector, vec(0, 0, 1))},
             {out("field", P::Field)}, [](NodeContext& ctx) {
               if (!check_direction(ctx, "normal", ctx.vector("normal"))) return;
               ctx.set("field", plane_field(ctx.vector("point"), ctx.vector("normal")));
             }));
  r.add(make("field.half_space", "field",
             "The half-space behind a plane (signed: negative against the normal)",
             {in("point", P::Vector, vec(0, 0, 0)), in("normal", P::Vector, vec(0, 0, 1))},
             {out("field", P::Field)}, [](NodeContext& ctx) {
               if (!check_direction(ctx, "normal", ctx.vector("normal"))) return;
               ctx.set("field", half_space_field(ctx.vector("point"), ctx.vector("normal")));
             }));
}

void add_field_ops(NodeRegistry& r) {
  using Binary = Field (*)(Field, Field);
  const std::pair<const char*, Binary> binaries[] = {
      {"field.union", &union_field},
      {"field.intersection", &intersection_field},
      {"field.difference", &difference_field},
      {"field.add", &add_field},
      {"field.multiply", &multiply_field},
  };
  for (const auto& [name, fn] : binaries) {
    const Binary f = fn;
    r.add(make(name, "field", {}, {in("a", P::Field), in("b", P::Field)}, {out("field", P::Field)},
               [f](NodeContext& ctx) { ctx.set("field", f(ctx.field("a"), ctx.field("b"))); }));
  }
  using Smooth = Field (*)(Field, Field, double);
  const std::pair<const char*, Smooth> smooth[] = {
      {"field.smooth_union", &smooth_union_field},
      {"field.smooth_intersection", &smooth_intersection_field},
      {"field.smooth_difference", &smooth_difference_field},
  };
  for (const auto& [name, fn] : smooth) {
    const Smooth f = fn;
    r.add(make(name, "field", "Blends within `radius`",
               {in("a", P::Field), in("b", P::Field), in("radius", P::Scalar, num(1))},
               {out("field", P::Field)}, [f](NodeContext& ctx) {
                 const double radius = ctx.scalar("radius");
                 if (!(radius >= 0.0) || !std::isfinite(radius)) {
                   ctx.infeasible({"radius"}, "the blend radius can't be negative");
                   return;
                 }
                 ctx.set("field", f(ctx.field("a"), ctx.field("b"), radius));
               }));
  }
  r.add(make("field.offset", "field", "Grows a region by `distance` (shrinks it when negative)",
             {in("field", P::Field), in("distance", P::Scalar, num(1))}, {out("field", P::Field)},
             [](NodeContext& ctx) {
               const double d = ctx.scalar("distance");
               if (!std::isfinite(d)) {
                 ctx.infeasible({"distance"}, "the distance must be finite");
                 return;
               }
               ctx.set("field", offset_field(ctx.field("field"), d));
             }));
  r.add(make("field.shell", "field", "The skin of a region, `thickness` deep inside its surface",
             {in("field", P::Field), in("thickness", P::Scalar, num(1))}, {out("field", P::Field)},
             [](NodeContext& ctx) {
               const double t = ctx.scalar("thickness");
               if (!check_size(ctx, "thickness", t)) return;
               ctx.set("field", shell_field(ctx.field("field"), t));
             }));
  r.add(make("field.negate", "field", "The complement of a region", {in("field", P::Field)},
             {out("field", P::Field)},
             [](NodeContext& ctx) { ctx.set("field", negate_field(ctx.field("field"))); }));
  r.add(make("field.abs", "field", "Unsigned distance to a region's surface", {in("field", P::Field)},
             {out("field", P::Field)},
             [](NodeContext& ctx) { ctx.set("field", abs_field(ctx.field("field"))); }));
  r.add(make("field.scale", "field", "Multiplies a field's values",
             {in("field", P::Field), in("factor", P::Scalar, num(1))}, {out("field", P::Field)},
             [](NodeContext& ctx) {
               const double f = ctx.scalar("factor");
               if (!std::isfinite(f)) {
                 ctx.infeasible({"factor"}, "the factor must be finite");
                 return;
               }
               ctx.set("field", scale_field(ctx.field("field"), f));
             }));
}

void add_math(NodeRegistry& r) {
  using Fn = double (*)(double, double);
  const std::pair<const char*, Fn> binaries[] = {
      {"math.add", [](double a, double b) { return a + b; }},
      {"math.subtract", [](double a, double b) { return a - b; }},
      {"math.multiply", [](double a, double b) { return a * b; }},
      {"math.min", [](double a, double b) { return std::min(a, b); }},
      {"math.max", [](double a, double b) { return std::max(a, b); }},
  };
  for (const auto& [name, fn] : binaries) {
    const Fn f = fn;
    r.add(make(name, "math", {}, {in("a", P::Scalar, num(0)), in("b", P::Scalar, num(0))},
               {out("value", P::Scalar)},
               [f](NodeContext& ctx) { ctx.set("value", f(ctx.scalar("a"), ctx.scalar("b"))); }));
  }
  r.add(make("math.divide", "math", {}, {in("a", P::Scalar, num(0)), in("b", P::Scalar, num(1))},
             {out("value", P::Scalar)}, [](NodeContext& ctx) {
               const double b = ctx.scalar("b");
               if (b == 0.0) {
                 ctx.infeasible({"b"}, "division by zero");
                 return;
               }
               ctx.set("value", ctx.scalar("a") / b);
             }));
  r.add(make("math.abs", "math", {}, {in("a", P::Scalar, num(0))}, {out("value", P::Scalar)},
             [](NodeContext& ctx) { ctx.set("value", std::abs(ctx.scalar("a"))); }));
  r.add(make("math.negate", "math", {}, {in("a", P::Scalar, num(0))}, {out("value", P::Scalar)},
             [](NodeContext& ctx) { ctx.set("value", -ctx.scalar("a")); }));
  r.add(make("math.sqrt", "math", {}, {in("a", P::Scalar, num(0))}, {out("value", P::Scalar)},
             [](NodeContext& ctx) {
               const double a = ctx.scalar("a");
               if (a < 0.0) {
                 ctx.infeasible({"a"}, "the square root of a negative number");
                 return;
               }
               ctx.set("value", std::sqrt(a));
             }));
  r.add(make("math.clamp", "math", {},
             {in("value", P::Scalar, num(0)), in("min", P::Scalar, num(0)), in("max", P::Scalar, num(1))},
             {out("value", P::Scalar)}, [](NodeContext& ctx) {
               const double lo = ctx.scalar("min");
               const double hi = ctx.scalar("max");
               if (lo > hi) {
                 ctx.infeasible({"min", "max"}, fmt::format("min {} is above max {}", lo, hi));
                 return;
               }
               ctx.set("value", std::clamp(ctx.scalar("value"), lo, hi));
             }));
  r.add(make("math.random", "math", "A seeded random number in [min, max)",
             {in("min", P::Scalar, num(0)), in("max", P::Scalar, num(1))}, {out("value", P::Scalar)},
             [](NodeContext& ctx) {
               const double lo = ctx.scalar("min");
               const double hi = ctx.scalar("max");
               if (lo > hi) {
                 ctx.infeasible({"min", "max"}, fmt::format("min {} is above max {}", lo, hi));
                 return;
               }
               ctx.set("value", lo + (hi - lo) * uniform(ctx.seed(), 0));
             }));
}

void add_logic(NodeRegistry& r) {
  r.add(make(
      "logic.compare", "logic", "1 when `a op b` holds, else 0",
      {in("a", P::Scalar, num(0)), in("b", P::Scalar, num(0))}, {out("value", P::Scalar)},
      [](NodeContext& ctx) {
        const double a = ctx.scalar("a");
        const double b = ctx.scalar("b");
        const std::string op = ctx.text("op");
        bool v = false;
        if (op == "less") v = a < b;
        else if (op == "less_equal") v = a <= b;
        else if (op == "greater") v = a > b;
        else if (op == "greater_equal") v = a >= b;
        else if (op == "equal") v = a == b;
        else v = a != b;
        ctx.set("value", v ? 1.0 : 0.0);
      },
      {{"op", PropertyKind::Choice, Setting(std::string("less")),
        {"less", "less_equal", "greater", "greater_equal", "equal", "not_equal"}, {}}}));
  r.add(make("logic.and", "logic", "1 when both are non-zero",
             {in("a", P::Scalar, num(0)), in("b", P::Scalar, num(0))}, {out("value", P::Scalar)},
             [](NodeContext& ctx) {
               ctx.set("value", ctx.scalar("a") != 0.0 && ctx.scalar("b") != 0.0 ? 1.0 : 0.0);
             }));
  r.add(make("logic.or", "logic", "1 when either is non-zero",
             {in("a", P::Scalar, num(0)), in("b", P::Scalar, num(0))}, {out("value", P::Scalar)},
             [](NodeContext& ctx) {
               ctx.set("value", ctx.scalar("a") != 0.0 || ctx.scalar("b") != 0.0 ? 1.0 : 0.0);
             }));
  r.add(make("logic.not", "logic", "1 when zero", {in("a", P::Scalar, num(0))},
             {out("value", P::Scalar)},
             [](NodeContext& ctx) { ctx.set("value", ctx.scalar("a") == 0.0 ? 1.0 : 0.0); }));
  r.add(make("logic.select", "logic", "`a` when the condition is non-zero, else `b`",
             {in("condition", P::Scalar, num(0)), in("a", P::Scalar, num(0)), in("b", P::Scalar, num(0))},
             {out("value", P::Scalar)}, [](NodeContext& ctx) {
               ctx.set("value", ctx.scalar("condition") != 0.0 ? ctx.scalar("a") : ctx.scalar("b"));
             }));
}

void add_vector(NodeRegistry& r) {
  r.add(make("vector.compose", "vector", {},
             {in("x", P::Scalar, num(0)), in("y", P::Scalar, num(0)), in("z", P::Scalar, num(0))},
             {out("vector", P::Vector)}, [](NodeContext& ctx) {
               ctx.set("vector", dvec3{ctx.scalar("x"), ctx.scalar("y"), ctx.scalar("z")});
             }));
  r.add(make("vector.decompose", "vector", {}, {in("vector", P::Vector, vec(0, 0, 0))},
             {out("x", P::Scalar), out("y", P::Scalar), out("z", P::Scalar)},
             [](NodeContext& ctx) {
               const dvec3 v = ctx.vector("vector");
               ctx.set("x", v.x);
               ctx.set("y", v.y);
               ctx.set("z", v.z);
             }));
  r.add(make("vector.add", "vector", {},
             {in("a", P::Vector, vec(0, 0, 0)), in("b", P::Vector, vec(0, 0, 0))},
             {out("vector", P::Vector)},
             [](NodeContext& ctx) { ctx.set("vector", ctx.vector("a") + ctx.vector("b")); }));
  r.add(make("vector.scale", "vector", {},
             {in("vector", P::Vector, vec(0, 0, 0)), in("factor", P::Scalar, num(1))},
             {out("vector", P::Vector)},
             [](NodeContext& ctx) { ctx.set("vector", ctx.vector("vector") * ctx.scalar("factor")); }));
  r.add(make("vector.length", "vector", {}, {in("vector", P::Vector, vec(0, 0, 0))},
             {out("value", P::Scalar)},
             [](NodeContext& ctx) { ctx.set("value", length(ctx.vector("vector"))); }));
}

void add_lists(NodeRegistry& r) {
  r.add(make("list.range", "list", "`count` numbers from `start`, `step` apart",
             {in("start", P::Scalar, num(0)), in("step", P::Scalar, num(1)), in("count", P::Scalar, num(1))},
             {out("list", P::List)}, [](NodeContext& ctx) {
               const double count = ctx.scalar("count");
               if (!(count >= 0.0) || count != std::floor(count) || count > 1e6) {
                 ctx.infeasible({"count"}, "the count must be a whole number from 0 to a million");
                 return;
               }
               List l;
               const double start = ctx.scalar("start");
               const double step = ctx.scalar("step");
               for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
                 l.push_back(start + step * static_cast<double>(i));
               }
               ctx.set("list", std::move(l));
             }));
  r.add(make("list.sum", "list", "Sums a list in order", {in("list", P::List)},
             {out("value", P::Scalar)}, [](NodeContext& ctx) {
               double s = 0.0;
               for (const double x : ctx.list("list")) s += x;
               ctx.set("value", s);
             }));
  r.add(make("list.count", "list", {}, {in("list", P::List)}, {out("value", P::Scalar)},
             [](NodeContext& ctx) {
               ctx.set("value", static_cast<double>(ctx.list("list").size()));
             }));
  r.add(make("list.get", "list", "The item at a zero-based index",
             {in("list", P::List), in("index", P::Scalar, num(0))}, {out("value", P::Scalar)},
             [](NodeContext& ctx) {
               const List& l = ctx.list("list");
               const double i = ctx.scalar("index");
               if (!(i >= 0.0) || i != std::floor(i) || i >= static_cast<double>(l.size())) {
                 ctx.infeasible({"list", "index"},
                                fmt::format("index {} isn't in a list of {}", i, l.size()));
                 return;
               }
               ctx.set("value", l[static_cast<std::size_t>(i)]);
             }));
}

void add_output(NodeRegistry& r) {
  r.add(make("output", "output", "The volume's contents", {in("contents", P::Field)}, {},
             [](NodeContext&) {},
             {{"cell_size", PropertyKind::Number, Setting(0.0), {},
               "the sampling grid's cell, mm (0: 64 cells along the boundary's longest side)"}}));
}

NodeRegistry make_library() {
  NodeRegistry r;
  add_inputs(r);
  add_queries(r);
  add_field_primitives(r);
  add_field_ops(r);
  add_math(r);
  add_logic(r);
  add_vector(r);
  add_lists(r);
  add_output(r);
  return r;
}

}  // namespace

const NodeRegistry& library() {
  static const NodeRegistry registry = make_library();
  return registry;
}

}  // namespace vs::proc

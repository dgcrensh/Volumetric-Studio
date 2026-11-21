#include "vs/proc/types.hpp"

#include <array>
#include <set>
#include <vector>

#include <fmt/format.h>

#include "vs/proc/distance.hpp"
#include "vs/proc/field.hpp"

namespace vs::proc {

namespace {

constexpr std::array<std::string_view, 8> kNames = {"scalar", "vector", "field",  "mesh",
                                                     "frames", "curves", "list",   "children"};

void append_transform(Hasher& h, const core::ftransform& t) {
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) h.add(t.linear(r, c));
  }
  h.add(t.translation);
}

std::size_t field_bytes(const FieldExpr* root) {
  std::set<const void*> seen;
  std::vector<const FieldExpr*> stack;
  if (root != nullptr) stack.push_back(root);
  std::size_t bytes = 0;
  while (!stack.empty()) {
    const FieldExpr* f = stack.back();
    stack.pop_back();
    if (!seen.insert(f).second) continue;
    bytes += sizeof(FieldExpr) + f->numbers().capacity() * sizeof(double) +
             f->children().capacity() * sizeof(Field);
    if (const TriangleDistance* m = f->mesh(); m != nullptr && seen.insert(m).second) {
      // Positions, triangles and about two BVH nodes per triangle.
      bytes += sizeof(TriangleDistance) + m->positions().capacity() * sizeof(dvec3) +
               m->triangle_count() * 160;
    }
    for (const Field& c : f->children()) {
      if (c) stack.push_back(c.get());
    }
  }
  return bytes;
}

}  // namespace

std::size_t value_bytes(const Value& value) {
  struct Visitor {
    std::size_t operator()(double) const { return sizeof(double); }
    std::size_t operator()(const dvec3&) const { return sizeof(dvec3); }
    std::size_t operator()(const Field& f) const { return field_bytes(f.get()); }
    std::size_t operator()(const MeshValue& m) const { return m ? m->memory_bytes() : 0; }
    std::size_t operator()(const Frames& f) const {
      return f ? f->frames.capacity() * sizeof(core::ftransform) : 0;
    }
    std::size_t operator()(const Curves& c) const {
      if (!c) return 0;
      std::size_t bytes = c->polylines.capacity() * sizeof(std::vector<fvec3>);
      for (const auto& p : c->polylines) bytes += p.capacity() * sizeof(fvec3);
      return bytes;
    }
    std::size_t operator()(const List& l) const { return l.capacity() * sizeof(double); }
    std::size_t operator()(const Children& c) const {
      if (!c) return 0;
      return c->children.size() * (sizeof(model::GeneratedChild) + 64) +
             c->surfaces.size() * sizeof(model::GeneratedSurface) +
             c->delegations.size() * sizeof(model::GeneratedDelegation);
    }
  };
  return sizeof(Value) + std::visit(Visitor{}, value);
}

std::string_view to_string(PortType type) { return kNames[static_cast<std::size_t>(type)]; }

std::optional<PortType> port_type_from_string(std::string_view text) {
  for (std::size_t i = 0; i < kNames.size(); ++i) {
    if (kNames[i] == text) return static_cast<PortType>(i);
  }
  return std::nullopt;
}

PortType type_of(const Value& value) { return static_cast<PortType>(value.index()); }

void hash_append(Hasher& h, const Value& value) {
  h.add(std::string_view("vstudio.proc.value"));
  h.add(static_cast<std::uint64_t>(value.index()));
  std::visit(
      [&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, double> || std::is_same_v<T, dvec3>) {
          h.add(v);
        } else if constexpr (std::is_same_v<T, Field>) {
          h.add(v != nullptr);
          if (v) h.add(v->hash());
        } else if constexpr (std::is_same_v<T, MeshValue>) {
          h.add(v != nullptr);
          if (!v) return;
          h.add(static_cast<std::uint64_t>(v->positions.size()));
          for (const auto& p : v->positions) h.add(p);
          h.add(static_cast<std::uint64_t>(v->indices.size()));
          for (const auto i : v->indices) h.add(i);
          for (const auto& id : v->triangle_volumes) h.add(id);
          for (const auto& id : v->seam_vertices) h.add(id);
        } else if constexpr (std::is_same_v<T, Frames>) {
          h.add(v != nullptr);
          if (!v) return;
          h.add(static_cast<std::uint64_t>(v->frames.size()));
          for (const auto& f : v->frames) append_transform(h, f);
        } else if constexpr (std::is_same_v<T, Curves>) {
          h.add(v != nullptr);
          if (!v) return;
          h.add(static_cast<std::uint64_t>(v->polylines.size()));
          for (const auto& line : v->polylines) {
            h.add(static_cast<std::uint64_t>(line.size()));
            for (const auto& p : line) h.add(p);
          }
        } else if constexpr (std::is_same_v<T, Children>) {
          h.add(v != nullptr);
          if (v) h.add(*v);
        } else {
          static_assert(std::is_same_v<T, List>);
          h.add(static_cast<std::uint64_t>(v.size()));
          for (const double x : v) h.add(x);
        }
      },
      value);
}

Hash256 hash_value(const Value& value) {
  Hasher h;
  h.add(value);
  return h.finalize();
}

std::string describe(const Value& value) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, double>) {
          return fmt::format("{}", v);
        } else if constexpr (std::is_same_v<T, dvec3>) {
          return fmt::format("({}, {}, {})", v.x, v.y, v.z);
        } else if constexpr (std::is_same_v<T, Field>) {
          return v ? fmt::format("field {}", v->hash().to_hex().substr(0, 12)) : "field (none)";
        } else if constexpr (std::is_same_v<T, MeshValue>) {
          return v ? fmt::format("mesh ({} triangles)", v->triangle_count()) : "mesh (none)";
        } else if constexpr (std::is_same_v<T, Frames>) {
          return v ? fmt::format("{} frames", v->frames.size()) : "frames (none)";
        } else if constexpr (std::is_same_v<T, Curves>) {
          return v ? fmt::format("{} curves", v->polylines.size()) : "curves (none)";
        } else if constexpr (std::is_same_v<T, Children>) {
          return v ? fmt::format("{} children, {} surfaces", v->children.size(),
                                 v->surfaces.size())
                   : "children (none)";
        } else {
          return fmt::format("list of {}", v.size());
        }
      },
      value);
}

}  // namespace vs::proc

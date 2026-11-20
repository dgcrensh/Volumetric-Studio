// Child volumes (L4): the nodes of a composite's generator. They make a
// model::GeneratedLayout (Children), which output.children hands to the
// engine; vs_eval expands it into the composite's level (generated.hpp).
//
// Keys. A child's key is derived from the node's id and its index, and a
// surface's from the node's id and the two indices, so a child keeps its
// id (and its cache entry) while the node keeps making it.

#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "node_helpers.hpp"

namespace vs::proc::detail {

namespace {

using model::GeneratedChild;
using model::GeneratedLayout;
using model::GeneratedSurface;

// The most children one node may make: each becomes a volume.
constexpr double kMaxChildren = 10000;

std::vector<PropertySpec> template_properties() {
  return {{"template", PropertyKind::Template, Setting(std::string()), {},
           "the composite's template each child is filled from (empty: placeholders)"},
          {"parameter", PropertyKind::TemplateParameter, Setting(std::string()), {},
           "the template parameter `values` sets, one value per child (empty: none)"}};
}

std::uint64_t child_seed(std::uint64_t seed, std::size_t index) {
  const Hash256 h = core::hash_of(std::string_view("vstudio.proc.child_seed"), seed,
                                  static_cast<std::uint64_t>(index));
  std::uint64_t s = 0;
  for (std::size_t i = 0; i < 8; ++i) s |= static_cast<std::uint64_t>(h.bytes[i]) << (8 * i);
  return s;
}

// Children named after the template (or "Cell"), numbered from 1, each with
// `values[k]` for the parameter when values are given. False when the
// values don't match the children.
bool fill_children(NodeContext& ctx, std::vector<core::dbox3> boxes, GeneratedLayout& out) {
  const std::string tmpl = ctx.text("template");
  const std::string parameter = ctx.text("parameter");
  const List& values = ctx.list("values");
  if (!values.empty()) {
    if (parameter.empty()) {
      ctx.infeasible({"values"}, "values are given but no template parameter takes them");
      return false;
    }
    if (values.size() != boxes.size()) {
      ctx.infeasible({"values"}, fmt::format("{} values for {} children; give one each",
                                             values.size(), boxes.size()));
      return false;
    }
  }
  const std::string stem = tmpl.empty() ? std::string("Cell") : tmpl;
  for (std::size_t k = 0; k < boxes.size(); ++k) {
    GeneratedChild c;
    c.key = Id::derive(ctx.node().id, static_cast<std::uint64_t>(k));
    c.name = fmt::format("{} {}", stem, k + 1);
    c.template_name = tmpl;
    c.box = boxes[k];
    c.seed = child_seed(ctx.seed(), k);
    if (!values.empty()) c.parameters.emplace(parameter, values[k]);
    out.children.push_back(std::move(c));
  }
  return true;
}

void set_children(NodeContext& ctx, GeneratedLayout layout) {
  const auto count = static_cast<double>(layout.children.size());
  ctx.set("children", Children(std::make_shared<const GeneratedLayout>(std::move(layout))));
  ctx.set("count", count);
}

double component(const dvec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

// The plane's two other axes, in the order its principal frame runs
// (GeneratedSurface): x: y, z; y: z, x; z: x, y.
std::array<int, 2> in_plane(int axis) {
  return {(axis + 1) % 3, (axis + 2) % 3};
}

// children.grid: the boundary's box split into nx * ny * nz cells.
void grid(NodeContext& ctx) {
  const dvec3 count = ctx.vector("count");
  for (int a = 0; a < 3; ++a) {
    if (!check_count(ctx, "count", component(count, a), 1, kMaxChildren)) return;
  }
  const std::array<std::size_t, 3> n{static_cast<std::size_t>(count.x),
                                     static_cast<std::size_t>(count.y),
                                     static_cast<std::size_t>(count.z)};
  if (static_cast<double>(n[0] * n[1] * n[2]) > kMaxChildren) {
    ctx.infeasible({"count"}, fmt::format("that makes {} children, more than the {:.0f} allowed",
                                          n[0] * n[1] * n[2], kMaxChildren));
    return;
  }
  const core::dbox3 box = ctx.inputs().bounds;
  // The cells' walls along each axis; the last one is the box's own.
  std::array<std::vector<double>, 3> walls;
  for (int a = 0; a < 3; ++a) {
    const double lo = component(box.min, a);
    const double hi = component(box.max, a);
    for (std::size_t i = 0; i <= n[a]; ++i) {
      walls[a].push_back(i == n[a] ? hi
                                   : lo + (hi - lo) * static_cast<double>(i) /
                                              static_cast<double>(n[a]));
    }
    if (!check_size(ctx, "count", walls[a][1] - walls[a][0])) return;
  }
  const auto index = [&](std::size_t i, std::size_t j, std::size_t k) {
    return i + n[0] * (j + n[1] * k);
  };
  std::vector<core::dbox3> boxes;
  for (std::size_t k = 0; k < n[2]; ++k) {
    for (std::size_t j = 0; j < n[1]; ++j) {
      for (std::size_t i = 0; i < n[0]; ++i) {
        boxes.push_back(core::dbox3::from_min_max({walls[0][i], walls[1][j], walls[2][k]},
                                                  {walls[0][i + 1], walls[1][j + 1],
                                                   walls[2][k + 1]}));
      }
    }
  }
  GeneratedLayout layout;
  if (!fill_children(ctx, boxes, layout)) return;

  // A surface between each pair of neighbours, inset by the margin so no two
  // seams share an edge.
  const std::string join = ctx.text("join");
  const double margin = ctx.number("margin");
  const double gap = ctx.number("gap");
  if (!(margin >= 0.0) || !std::isfinite(margin)) {
    ctx.infeasible({"margin"}, "the margin must be 0 or more");
    return;
  }
  if (join == "clearance" && !check_size(ctx, "gap", gap)) return;
  if (join == "none") {
    set_children(ctx, std::move(layout));
    return;
  }
  for (std::size_t k = 0; k < n[2]; ++k) {
    for (std::size_t j = 0; j < n[1]; ++j) {
      for (std::size_t i = 0; i < n[0]; ++i) {
        const std::array<std::size_t, 3> at{i, j, k};
        for (int a = 0; a < 3; ++a) {
          if (at[a] + 1 >= n[a]) continue;
          std::array<std::size_t, 3> next = at;
          ++next[a];
          GeneratedSurface s;
          s.a = index(at[0], at[1], at[2]);
          s.b = index(next[0], next[1], next[2]);
          s.key = Id::derive(ctx.node().id, fmt::format("join:{}:{}", s.a, s.b));
          s.name = fmt::format("{} / {}", layout.children[s.a].name, layout.children[s.b].name);
          s.type = join == "clearance" ? model::ConnectionType::Clearance
                                       : model::ConnectionType::Bonded;
          s.gap = gap;
          s.axis = a;
          s.offset = walls[a][next[a]];
          const auto axes = in_plane(a);
          for (int q = 0; q < 2; ++q) {
            const int b = axes[q];
            s.lo[q] = walls[b][at[b]] + margin;
            s.hi[q] = walls[b][at[b] + 1] - margin;
            if (!check_size(ctx, "margin", s.hi[q] - s.lo[q])) return;
          }
          layout.surfaces.push_back(std::move(s));
        }
      }
    }
  }
  set_children(ctx, std::move(layout));
}

// children.at_frames: a box of `size` centred at each frame's origin.
void at_frames(NodeContext& ctx) {
  const dvec3 size = ctx.vector("size");
  if (!check_size(ctx, "size", size.x) || !check_size(ctx, "size", size.y) ||
      !check_size(ctx, "size", size.z)) {
    return;
  }
  const auto& frames = ctx.frames("frames")->frames;
  if (static_cast<double>(frames.size()) > kMaxChildren) {
    ctx.infeasible({"frames"}, fmt::format("{} frames make more than the {:.0f} children allowed",
                                           frames.size(), kMaxChildren));
    return;
  }
  std::vector<core::dbox3> boxes;
  for (const auto& f : frames) {
    boxes.push_back(core::dbox3::from_center_half_extent(core::to_fp64(f.translation), size * 0.5));
  }
  GeneratedLayout layout;
  if (!fill_children(ctx, std::move(boxes), layout)) return;
  set_children(ctx, std::move(layout));
}

// children.merge: b's children after a's.
void merge(NodeContext& ctx) {
  GeneratedLayout out = *ctx.children("a");
  const GeneratedLayout& b = *ctx.children("b");
  const std::size_t offset = out.children.size();
  out.children.insert(out.children.end(), b.children.begin(), b.children.end());
  for (auto s : b.surfaces) {
    s.a += offset;
    s.b += offset;
    out.surfaces.push_back(std::move(s));
  }
  for (auto d : b.delegations) {
    d.child += offset;
    out.delegations.push_back(d);
  }
  set_children(ctx, std::move(out));
}

// children.delegate: the surface goes to the child with a face on its patch
// containing the patch's centre.
void delegate(NodeContext& ctx) {
  const ConnectionInput* surface = surface_of(ctx);
  if (surface == nullptr) return;
  GeneratedLayout out = *ctx.children("children");
  int axis = -1;
  for (int a = 0; a < 3; ++a) {
    if (std::abs(std::abs(component(surface->normal, a)) - 1.0) < 1e-9) axis = a;
  }
  if (axis < 0 || !surface->patch) {
    ctx.fail(fmt::format("surface '{}' isn't on a plane square to an axis; generated children "
                         "are boxes, so they meet such planes only",
                         surface->name));
    return;
  }
  const core::dbox3 patch = surface->patch->bounds();
  const dvec3 centre = (patch.min + patch.max) * 0.5;
  const double plane = component(surface->origin, axis);
  const double tol = ctx.settings().linear_tolerance_mm * 10.0;
  for (std::size_t k = 0; k < out.children.size(); ++k) {
    const core::dbox3& b = out.children[k].box;
    const bool on_face = std::abs(component(b.min, axis) - plane) <= tol ||
                         std::abs(component(b.max, axis) - plane) <= tol;
    if (!on_face) continue;
    bool inside = true;
    for (const int q : in_plane(axis)) {
      const double c = component(centre, q);
      inside = inside && c >= component(b.min, q) && c <= component(b.max, q);
    }
    if (!inside) continue;
    out.delegations.push_back({surface->id, k});
    set_children(ctx, std::move(out));
    return;
  }
  ctx.infeasible({"children"},
                 fmt::format("no child has a face on surface '{}' at its centre", surface->name));
}

}  // namespace

void add_children_nodes(NodeRegistry& r) {
  const auto children_out = [] {
    return std::vector<PortSpec>{out("children", P::Children, "the child volumes"),
                                 out("count", P::Scalar, "how many there are")};
  };

  {
    auto props = template_properties();
    props.push_back({"join", PropertyKind::Choice, Setting(std::string("bonded")),
                     {"bonded", "clearance", "none"},
                     "how neighbouring cells are joined: a connection surface of this type, or "
                     "none (they then touch undeclared)"});
    props.push_back({"gap", PropertyKind::Number, num(0.2), {}, "a clearance join's gap, mm"});
    props.push_back({"margin", PropertyKind::Number, num(1.0), {},
                     "how far each join's patch stays inside the shared face's edges, mm"});
    r.add(make("children.grid", "children",
               "The composite's boundary box split into a grid of child volumes, neighbours "
               "joined by connection surfaces",
               {in("count", P::Vector, vec(2, 1, 1), "cells along x, y and z"),
                optional_in("values", P::List, "the template parameter's value for each cell, "
                                               "x fastest")},
               children_out(), grid, std::move(props), false));
  }
  r.add(make("children.at_frames", "children",
             "A child volume, an axis-aligned box, centred at each frame (its rotation is "
             "ignored)",
             {in("frames", P::Frames, std::nullopt, "where the children go"),
              in("size", P::Vector, vec(10, 10, 10), "each box's size, mm"),
              optional_in("values", P::List, "the template parameter's value for each child")},
             children_out(), at_frames, template_properties()));
  r.add(make("children.merge", "children", "Two sets of child volumes as one",
             {in("a", P::Children), in("b", P::Children)}, children_out(), merge));
  r.add(make("children.delegate", "children",
             "Passes a connection surface joining the composite to the child lying against it",
             {in("children", P::Children)}, children_out(), delegate,
             {{"surface", PropertyKind::Surface, std::nullopt, {}, "the surface's id"}}, false));
  r.add(make("output.children", "output",
             "The composite's generated children: checked, then built like authored ones",
             {in("children", P::Children)}, {}, [](NodeContext&) {}));
}

}  // namespace vs::proc::detail

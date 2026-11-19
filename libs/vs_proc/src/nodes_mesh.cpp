// Mesh nodes (plan, M12): primitives, instancing, measuring, and booleans
// through Manifold.
//
// Meshes inside a graph are tools: a volume's contents are a field, so a
// mesh reaches them through convert.mesh_to_field (decision 0012).

#include <cmath>
#include <memory>
#include <numbers>
#include <string>

#include <fmt/format.h>

#include "mesh_boolean.hpp"
#include "node_helpers.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/proc/field.hpp"

namespace vs::proc::detail {

namespace {

// The most triangles a mesh node may make.
constexpr double kMaxTriangles = 20'000'000;

mesh::Mesh box_mesh(const dvec3& centre, const dvec3& size, const Id& volume) {
  mesh::Mesh m;
  const dvec3 h = size * 0.5;
  for (int k = 0; k < 8; ++k) {
    const dvec3 corner{(k & 1) ? h.x : -h.x, (k & 2) ? h.y : -h.y, (k & 4) ? h.z : -h.z};
    m.add_vertex(core::to_fp32(centre + corner));
  }
  // Two triangles a face, counter-clockwise from outside.
  const std::uint32_t faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                     {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (const auto& f : faces) {
    m.add_triangle(f[0], f[1], f[2], volume);
    m.add_triangle(f[0], f[2], f[3], volume);
  }
  return m;
}

mesh::Mesh cylinder_mesh(const dvec3& centre, const dvec3& axis_dir, double radius, double height,
                         std::uint32_t segments, const Id& volume) {
  const dvec3 z = normalize(axis_dir);
  const dvec3 helper = std::abs(z.x) < 0.9 ? dvec3{1, 0, 0} : dvec3{0, 1, 0};
  const dvec3 x = normalize(helper - z * dot(helper, z));
  const dvec3 y = cross(z, x);
  const dvec3 bottom = centre - z * (0.5 * height);
  const dvec3 top = centre + z * (0.5 * height);
  mesh::Mesh m;
  for (std::uint32_t i = 0; i < segments; ++i) {
    const double a = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(segments);
    const dvec3 r = (x * cos_det(a) + y * sin_det(a)) * radius;
    m.add_vertex(core::to_fp32(bottom + r));  // 2i
    m.add_vertex(core::to_fp32(top + r));     // 2i + 1
  }
  const std::uint32_t cb = m.add_vertex(core::to_fp32(bottom));
  const std::uint32_t ct = m.add_vertex(core::to_fp32(top));
  for (std::uint32_t i = 0; i < segments; ++i) {
    const std::uint32_t j = (i + 1) % segments;
    const std::uint32_t b0 = 2 * i, t0 = 2 * i + 1, b1 = 2 * j, t1 = 2 * j + 1;
    m.add_triangle(b0, b1, t1, volume);
    m.add_triangle(b0, t1, t0, volume);
    m.add_triangle(cb, b1, b0, volume);
    m.add_triangle(ct, t0, t1, volume);
  }
  return m;
}

void boolean_node(NodeRegistry& r, const char* name, const char* doc, BooleanOp op) {
  r.add(make(name, "mesh", doc, {in("a", P::Mesh), in("b", P::Mesh)},
             {out("mesh", P::Mesh), out("triangles", P::Scalar)}, [op](NodeContext& ctx) {
               const MeshValue& a = ctx.mesh("a");
               const MeshValue& b = ctx.mesh("b");
               const mesh::Mesh none;
               BooleanResult res = mesh_boolean(a ? *a : none, b ? *b : none, op,
                                                ctx.inputs().volume);
               if (res.bad_input != 0) {
                 const char* port = res.bad_input == 1 ? "a" : "b";
                 ctx.infeasible({port}, fmt::format("mesh {} can't take part in a boolean: {}",
                                                    port, res.message));
                 return;
               }
               if (!res.mesh) {
                 ctx.fail(fmt::format("the boolean failed: {}", res.message));
                 return;
               }
               const auto triangles = static_cast<double>(res.mesh->triangle_count());
               ctx.set("mesh", MeshValue(std::make_shared<mesh::Mesh>(std::move(*res.mesh))));
               ctx.set("triangles", triangles);
             }));
}

}  // namespace

void add_mesh_nodes(NodeRegistry& r) {
  r.add(make("mesh.box", "mesh", "A box mesh (12 triangles), axis-aligned",
             {in("centre", P::Vector, vec(0, 0, 0)), in("size", P::Vector, vec(10, 10, 10))},
             {out("mesh", P::Mesh)}, [](NodeContext& ctx) {
               const dvec3 size = ctx.vector("size");
               const double min = ctx.settings().min_feature_size_mm();
               if (!(size.x >= min && size.y >= min && size.z >= min) || !std::isfinite(length(size))) {
                 ctx.infeasible({"size"}, fmt::format("every side must be at least {} mm", min));
                 return;
               }
               ctx.set("mesh", MeshValue(std::make_shared<mesh::Mesh>(
                                   box_mesh(ctx.vector("centre"), size, ctx.inputs().volume))));
             }));

  r.add(make("mesh.cylinder", "mesh",
             "A capped cylinder mesh centred on `centre` along `axis`, `segments` round",
             {in("centre", P::Vector, vec(0, 0, 0)), in("axis", P::Vector, vec(0, 0, 1)),
              in("radius", P::Scalar, num(5)), in("height", P::Scalar, num(10)),
              in("segments", P::Scalar, num(32))},
             {out("mesh", P::Mesh)}, [](NodeContext& ctx) {
               const double radius = ctx.scalar("radius");
               const double height = ctx.scalar("height");
               const double segments = ctx.scalar("segments");
               if (!check_direction(ctx, "axis", ctx.vector("axis"))) return;
               if (!check_size(ctx, "radius", radius) || !check_size(ctx, "height", height)) return;
               if (!check_count(ctx, "segments", segments, 3, 4096)) return;
               ctx.set("mesh", MeshValue(std::make_shared<mesh::Mesh>(cylinder_mesh(
                                   ctx.vector("centre"), ctx.vector("axis"), radius, height,
                                   static_cast<std::uint32_t>(segments), ctx.inputs().volume))));
             }));

  r.add(make("mesh.instances", "instance",
             "Places a template mesh at every frame (copies, not merged: use mesh.union to join "
             "overlapping ones)",
             {in("template", P::Mesh), in("frames", P::Frames)},
             {out("mesh", P::Mesh), out("triangles", P::Scalar)}, [](NodeContext& ctx) {
               const MeshValue& t = ctx.mesh("template");
               const Frames& frames = ctx.frames("frames");
               if (!t || t->empty()) {
                 ctx.infeasible({"template"}, "the template mesh is empty");
                 return;
               }
               const double total =
                   static_cast<double>(t->triangle_count()) * static_cast<double>(frames->frames.size());
               if (total > kMaxTriangles) {
                 ctx.infeasible({"template", "frames"},
                                fmt::format("{:.0f} triangles is more than the {:.0f} allowed",
                                            total, kMaxTriangles));
                 return;
               }
               auto m = std::make_shared<mesh::Mesh>();
               for (const auto& f : frames->frames) {
                 const core::dtransform x = core::to_fp64(f);
                 const auto base = static_cast<std::uint32_t>(m->vertex_count());
                 for (const auto& p : t->positions) {
                   m->add_vertex(core::to_fp32(x.apply_point(core::to_fp64(p))));
                 }
                 for (std::size_t k = 0; k < t->triangle_count(); ++k) {
                   const auto tri = t->triangle(k);
                   m->add_triangle(base + tri[0], base + tri[1], base + tri[2], ctx.inputs().volume);
                 }
               }
               const auto triangles = static_cast<double>(m->triangle_count());
               ctx.set("mesh", MeshValue(std::move(m)));
               ctx.set("triangles", triangles);
             }));

  r.add(make("mesh.measure", "mesh",
             "A mesh's enclosed volume (mm³, for a closed mesh), surface area (mm²) and triangle "
             "count",
             {in("mesh", P::Mesh)},
             {out("volume", P::Scalar), out("area", P::Scalar), out("triangles", P::Scalar)},
             [](NodeContext& ctx) {
               const MeshValue& m = ctx.mesh("mesh");
               double area = 0.0;
               for (std::size_t t = 0; t < m->triangle_count(); ++t) area += mesh::double_area(*m, t);
               ctx.set("volume", mesh::enclosed_volume(*m));
               ctx.set("area", 0.5 * area);
               ctx.set("triangles", static_cast<double>(m->triangle_count()));
             }));

  boolean_node(r, "mesh.union", "The union of two closed meshes (Manifold)", BooleanOp::Union);
  boolean_node(r, "mesh.difference", "`a` less `b`, both closed meshes (Manifold)",
               BooleanOp::Difference);
  boolean_node(r, "mesh.intersection", "Where two closed meshes overlap (Manifold)",
               BooleanOp::Intersection);
}

}  // namespace vs::proc::detail

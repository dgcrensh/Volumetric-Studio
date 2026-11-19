#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include <variant>

#include "vs/kernel/tessellate.hpp"

namespace vs::app {

namespace {

using core::dvec3;

// Indexed triangles with one normal per vertex, split where the surface
// creases (see the header). `positions` and `triangles` describe one
// surface patch; the result is appended to `out`.
void append_smoothed(const std::vector<dvec3>& positions,
                     const std::vector<std::array<std::uint32_t, 3>>& triangles, double crease_angle,
                     DisplayMesh& out) {
  const std::size_t nv = positions.size();
  const std::size_t nt = triangles.size();
  // Face normals, unnormalised (length = twice the area), and unit.
  std::vector<dvec3> weighted(nt), unit(nt);
  for (std::size_t t = 0; t < nt; ++t) {
    const auto& tri = triangles[t];
    const dvec3 n = cross(positions[tri[1]] - positions[tri[0]], positions[tri[2]] - positions[tri[0]]);
    weighted[t] = n;
    unit[t] = normalize(n);
  }
  // Triangles around each vertex (CSR).
  std::vector<std::uint32_t> start(nv + 1, 0);
  for (const auto& tri : triangles) {
    for (const auto v : tri) ++start[v + 1];
  }
  for (std::size_t v = 0; v < nv; ++v) start[v + 1] += start[v];
  std::vector<std::uint32_t> around(start[nv]);
  {
    std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
    for (std::size_t t = 0; t < nt; ++t) {
      for (const auto v : triangles[t]) around[fill[v]++] = static_cast<std::uint32_t>(t);
    }
  }

  const double min_cos = std::cos(crease_angle);
  // corner[3 t + k]: the output vertex of triangle t's k-th corner.
  std::vector<std::uint32_t> corner(3 * nt, 0);
  struct Cluster {
    dvec3 first;  // the unit normal that opened the cluster
    dvec3 sum;    // area-weighted normal sum
    std::uint32_t index;
  };
  std::vector<Cluster> clusters;
  for (std::size_t v = 0; v < nv; ++v) {
    clusters.clear();
    // Triangles with an area take part first; degenerate ones (a sphere's
    // poles) have no normal of their own and join the first cluster.
    for (int pass = 0; pass < 2; ++pass)
    for (std::uint32_t a = start[v]; a < start[v + 1]; ++a) {
      const std::uint32_t t = around[a];
      const bool degenerate = length_squared(unit[t]) == 0.0;
      if (degenerate != (pass == 1)) continue;
      Cluster* match = nullptr;
      if (degenerate && !clusters.empty()) match = &clusters.front();
      for (auto& c : clusters) {
        if (match != nullptr) break;
        if (dot(c.first, unit[t]) >= min_cos) {
          match = &c;
          break;
        }
      }
      if (match == nullptr) {
        clusters.push_back({unit[t], {}, static_cast<std::uint32_t>(out.positions.size())});
        out.positions.push_back(core::to_fp32(positions[v]));
        out.normals.push_back({});
        match = &clusters.back();
      }
      match->sum = match->sum + weighted[t];
      for (int k = 0; k < 3; ++k) {
        if (triangles[t][static_cast<std::size_t>(k)] == v) corner[3 * t + static_cast<std::size_t>(k)] = match->index;
      }
    }
    for (const auto& c : clusters) {
      dvec3 n = normalize(c.sum);
      if (length_squared(n) == 0.0) n = normalize(c.first);
      if (length_squared(n) == 0.0) n = {0.0, 0.0, 1.0};
      out.normals[c.index] = core::to_fp32(n);
    }
  }
  out.indices.insert(out.indices.end(), corner.begin(), corner.end());
}

}  // namespace

fbox3 DisplayMesh::bounds() const {
  fbox3 box;
  for (const auto& p : positions) box.expand(p);
  for (const auto& line : edges) {
    for (const auto& p : line) box.expand(p);
  }
  return box;
}

std::string_view to_string(ItemRole role) {
  switch (role) {
    case ItemRole::Boundary: return "boundary";
    case ItemRole::Contents: return "contents";
    case ItemRole::ConnectionSurface: return "connection surface";
    case ItemRole::LayoutSolid: return "layout solid";
    case ItemRole::Sketch: return "sketch";
  }
  return "?";
}

const SceneItem* Scene::find(const Id& subject, ItemRole role) const {
  for (const auto& item : items) {
    if (item.subject == subject && item.role == role) return &item;
  }
  return nullptr;
}

std::size_t Scene::count(ItemRole role) const {
  std::size_t n = 0;
  for (const auto& item : items) n += item.role == role ? 1 : 0;
  return n;
}

Rgb volume_color(std::size_t index) {
  // Muted, distinguishable hues that leave green, blue and orange to the
  // connection surface types.
  static constexpr std::array<Rgb, 6> kPalette{{
      {0.42f, 0.56f, 0.78f},  // steel blue
      {0.80f, 0.55f, 0.45f},  // clay
      {0.62f, 0.50f, 0.75f},  // lavender
      {0.50f, 0.70f, 0.66f},  // sage
      {0.82f, 0.72f, 0.45f},  // sand
      {0.75f, 0.50f, 0.62f},  // rose
  }};
  return kPalette[index % kPalette.size()];
}

Rgb connection_color(model::ConnectionType type) {
  switch (type) {
    case model::ConnectionType::Bonded: return {0.20f, 0.72f, 0.30f};
    case model::ConnectionType::Clearance: return {0.20f, 0.55f, 0.95f};
    case model::ConnectionType::BoltedFlange: return {0.98f, 0.55f, 0.10f};
  }
  return {1.0f, 0.0f, 1.0f};
}

core::Result<DisplayMesh> display_mesh(const kernel::Shape& shape, const core::ProductSettings& settings,
                                       const SceneOptions& options) {
  kernel::TessellationParams params;
  params.deflection = options.deflection;
  params.angle = options.angle;
  auto tess = kernel::tessellate(shape, params, settings);
  if (!tess) return tess.error();
  DisplayMesh out;
  for (const auto& face : tess->faces) {
    append_smoothed(face.points, face.triangles, options.crease_angle, out);
  }
  for (const auto& edge : tess->edges) {
    if (edge.degenerate || edge.points.size() < 2) continue;
    std::vector<fvec3> line;
    line.reserve(edge.points.size());
    for (const auto& p : edge.points) line.push_back(core::to_fp32(p));
    out.edges.push_back(std::move(line));
  }
  return out;
}

DisplayMesh display_mesh(const mesh::Mesh& mesh, const SceneOptions& options) {
  std::vector<dvec3> positions;
  positions.reserve(mesh.positions.size());
  for (const auto& p : mesh.positions) positions.push_back(core::to_fp64(p));
  std::vector<std::array<std::uint32_t, 3>> triangles;
  triangles.reserve(mesh.triangle_count());
  for (std::size_t t = 0; t < mesh.triangle_count(); ++t) triangles.push_back(mesh.triangle(t));
  DisplayMesh out;
  append_smoothed(positions, triangles, options.crease_angle, out);
  return out;
}

Scene build_scene(const eval::Evaluation& evaluation, const core::ProductSettings& settings,
                  const SceneOptions& options) {
  Scene scene;
  const model::Document& doc = evaluation.document;
  auto add = [&](SceneItem item) {
    if (item.mesh.empty() && item.mesh.edges.empty()) return;
    scene.bounds.expand(item.mesh.bounds());
    scene.items.push_back(std::move(item));
  };
  auto shape_item = [&](const kernel::Shape& shape, const core::SubjectRef& subject) {
    auto m = display_mesh(shape, settings, options);
    if (!m) {
      core::Diagnostic d = m.error();
      d.subjects.insert(d.subjects.begin(), subject);
      scene.diagnostics.add(std::move(d));
      return DisplayMesh{};
    }
    return std::move(m).value();
  };

  for (std::size_t i = 0; i < doc.volumes.size(); ++i) {
    const model::Volume& volume = doc.volumes[i];
    const model::VolumeBuild* vb = evaluation.build.find_volume(volume.id);
    const eval::VolumeResult* result = evaluation.find(volume.id);
    const Rgb color = volume.keep_out ? Rgb{0.85f, 0.25f, 0.25f} : volume_color(i);
    const bool has_contents = result != nullptr && result->contents &&
                              (result->contents->mesh || !result->contents->solid.is_empty());

    if (vb != nullptr && !vb->boundary.is_empty()) {
      SceneItem item;
      item.role = ItemRole::Boundary;
      item.subject = volume.id;
      item.volume = volume.id;
      item.mesh = shape_item(vb->boundary, doc.boundary_subject(volume.id));
      item.color = color;
      // A placeholder's boundary is all there is to see; over contents it
      // is a faint tint.
      item.transparency = has_contents ? 0.88f : 0.6f;
      add(std::move(item));
    }
    if (has_contents) {
      const eval::GeneratedContents& c = *result->contents;
      SceneItem item;
      item.role = ItemRole::Contents;
      item.subject = volume.id;
      item.volume = volume.id;
      item.mesh = c.mesh ? display_mesh(*c.mesh, options)
                         : shape_item(c.solid, doc.volume_subject(volume.id));
      item.stale = result->stale_contents;
      item.color = item.stale ? kStaleColor : color;
      add(std::move(item));
    }
  }

  for (const model::ConnectionSurface& surface : doc.connections) {
    const model::ConnectionBuild* cb = evaluation.build.find_connection(surface.id);
    if (cb == nullptr || !cb->ok || cb->patch.is_empty()) continue;
    SceneItem item;
    item.role = ItemRole::ConnectionSurface;
    item.subject = surface.id;
    item.mesh = shape_item(cb->patch, doc.connection_subject(surface.id));
    item.color = connection_color(surface.type());
    item.transparency = 0.25f;
    add(std::move(item));
  }

  // --- authoring: layout solids no boundary uses, and sketches.
  std::set<Id> in_boundaries;
  for (const auto& v : doc.volumes) {
    if (const auto* fb = std::get_if<model::FeatureBoundary>(&v.boundary)) {
      in_boundaries.insert(fb->features.begin(), fb->features.end());
    }
  }
  for (const auto& k : doc.keep_outs) in_boundaries.insert(k.cutters.begin(), k.cutters.end());
  for (const auto& r : evaluation.build.layout.features) {
    if (!r.ok() || r.tool.is_empty() || r.operation != feature::Operation::Add) continue;
    if (in_boundaries.count(r.feature) != 0) continue;
    SceneItem item;
    item.role = ItemRole::LayoutSolid;
    item.subject = r.feature;
    item.mesh = shape_item(r.tool, doc.layout.subject(r.feature));
    item.color = kLayoutColor;
    item.transparency = 0.55f;
    add(std::move(item));
  }

  // Sketches some feature or connection surface uses are secondary.
  auto used_sketches = [&](const feature::FeatureTree& tree) {
    std::set<Id> used;
    for (const auto& f : tree.features()) {
      if (const auto* e = f.as<feature::ExtrudeFeature>()) used.insert(e->sketch);
      if (const auto* r = f.as<feature::RevolveFeature>()) used.insert(r->sketch);
      if (const auto* s = f.as<feature::SweepFeature>()) used.insert({s->profile, s->path});
      if (const auto* l = f.as<feature::LoftFeature>()) used.insert(l->sections.begin(), l->sections.end());
    }
    return used;
  };
  auto add_sketches = [&](const feature::FeatureTree& tree, const feature::RebuildResult& rebuild,
                          const Id& volume, std::set<Id> used) {
    for (const auto& r : rebuild.features) {
      if (!r.ok() || !r.sketch || tree.find(r.feature) == nullptr) continue;
      SceneItem item;
      item.role = ItemRole::Sketch;
      item.subject = r.feature;
      item.volume = volume;
      item.mesh.edges = sketch_lines(*r.sketch);
      item.color = kSketchColor;
      item.secondary = used.count(r.feature) != 0;
      if (item.mesh.edges.empty()) continue;
      // Secondary sketches don't take part in framing the view.
      if (item.secondary) {
        scene.items.push_back(std::move(item));
      } else {
        add(std::move(item));
      }
    }
  };
  {
    std::set<Id> used = used_sketches(doc.layout);
    for (const auto& s : doc.connections) {
      if (const auto* sp = std::get_if<model::SketchPatch>(&s.geometry)) used.insert(sp->sketch);
    }
    add_sketches(doc.layout, evaluation.build.layout, {}, std::move(used));
  }
  for (const auto& v : doc.volumes) {
    const feature::FeatureTree* tree = v.tree();
    if (tree == nullptr) continue;
    const eval::VolumeResult* result = evaluation.find(v.id);
    const feature::RebuildResult* rebuild = nullptr;
    if (result != nullptr && result->contents && result->contents->rebuild) rebuild = &*result->contents->rebuild;
    if (rebuild == nullptr) {
      if (const auto* vb = evaluation.build.find_volume(v.id); vb != nullptr && vb->contents_rebuild) {
        rebuild = &*vb->contents_rebuild;
      }
    }
    if (rebuild != nullptr) add_sketches(*tree, *rebuild, v.id, used_sketches(*tree));
  }
  return scene;
}

std::vector<std::vector<fvec3>> sketch_lines(const sketch::Sketch& sketch) {
  std::vector<std::vector<fvec3>> out;
  const sketch::SketchPlane& plane = sketch.plane();
  auto model = [&](core::dvec2 p) { return core::to_fp32(plane.to_model(p)); };
  auto arc = [&](core::dvec2 c, double r, double a0, double a1) {
    const double sweep = a1 - a0;
    const int n = std::max(8, static_cast<int>(std::ceil(std::abs(sweep) / (2.0 * 3.14159265358979 / 96.0))));
    std::vector<fvec3> line;
    line.reserve(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) {
      const double a = a0 + sweep * static_cast<double>(i) / n;
      line.push_back(model({c.x + r * std::cos(a), c.y + r * std::sin(a)}));
    }
    return line;
  };
  for (const auto& e : sketch.entities()) {
    switch (e.kind()) {
      case sketch::EntityKind::Point: break;
      case sketch::EntityKind::Line:
        if (const auto g = sketch::line_geometry(sketch, e.id)) out.push_back({model(g->start), model(g->end)});
        break;
      case sketch::EntityKind::Circle:
        if (const auto g = sketch::circle_geometry(sketch, e.id)) {
          out.push_back(arc(g->center, g->radius, 0.0, 2.0 * 3.14159265358979));
        }
        break;
      case sketch::EntityKind::Arc:
        if (const auto g = sketch::arc_geometry(sketch, e.id)) {
          out.push_back(arc(g->center, g->radius, g->start_angle, g->end_angle));
        }
        break;
    }
  }
  return out;
}

}  // namespace vs::app

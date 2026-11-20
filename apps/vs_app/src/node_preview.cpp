#include "node_preview.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace vs::app {

namespace {

Rgb mix(const Rgb& a, const Rgb& b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// A diamond (octahedron) of radius `r` at each point, flat-shaded.
DisplayMesh glyphs(const std::vector<fvec3>& points, float r) {
  DisplayMesh m;
  static constexpr std::array<std::array<int, 3>, 8> kFaces{{{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
                                                             {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}}};
  const std::array<fvec3, 6> dirs{fvec3{1, 0, 0}, fvec3{-1, 0, 0}, fvec3{0, 1, 0},
                                  fvec3{0, -1, 0}, fvec3{0, 0, 1},  fvec3{0, 0, -1}};
  m.positions.reserve(points.size() * 24);
  for (const fvec3& p : points) {
    for (const auto& f : kFaces) {
      const fvec3 a = dirs[static_cast<std::size_t>(f[0])];
      const fvec3 b = dirs[static_cast<std::size_t>(f[1])];
      const fvec3 c = dirs[static_cast<std::size_t>(f[2])];
      fvec3 n{a.x + b.x + c.x, a.y + b.y + c.y, a.z + b.z + c.z};
      const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
      n = {n.x / len, n.y / len, n.z / len};
      for (const fvec3& d : {a, b, c}) {
        m.indices.push_back(static_cast<std::uint32_t>(m.positions.size()));
        m.positions.push_back({p.x + d.x * r, p.y + d.y * r, p.z + d.z * r});
        m.normals.push_back(n);
      }
    }
  }
  return m;
}

// One z level of the samples as a coloured grid, with the zero contour.
DisplayMesh slice(const proc::SampledField& s, double where) {
  DisplayMesh m;
  const auto& g = s.grid;
  if (g.nx < 2 || g.ny < 2 || g.nz < 1 || s.values.size() != g.size()) return m;
  const auto k = static_cast<std::uint32_t>(
      std::clamp(std::lround(where * static_cast<double>(g.nz - 1)), 0L, static_cast<long>(g.nz - 1)));
  // Colour scale: the largest |value| on the level, so the slice shows
  // its shape at any scale.
  float range = 0.0f;
  for (std::uint32_t j = 0; j < g.ny; ++j) {
    for (std::uint32_t i = 0; i < g.nx; ++i) range = std::max(range, std::abs(s.at(i, j, k)));
  }
  if (range <= 0.0f) range = 1.0f;
  for (std::uint32_t j = 0; j < g.ny; ++j) {
    for (std::uint32_t i = 0; i < g.nx; ++i) {
      const core::dvec3 p = g.point(i, j, k);
      m.positions.push_back(core::to_fp32(p));
      m.normals.push_back({0, 0, 1});
      const float v = std::clamp(s.at(i, j, k) / range, -1.0f, 1.0f);
      // Strong near zero so the surface stands out.
      const float t = std::pow(std::abs(v), 0.5f);
      m.colors.push_back(mix(Rgb{1, 1, 1}, v < 0 ? kPreviewInside : kPreviewOutside, t));
    }
  }
  auto at = [&](std::uint32_t i, std::uint32_t j) { return j * g.nx + i; };
  for (std::uint32_t j = 0; j + 1 < g.ny; ++j) {
    for (std::uint32_t i = 0; i + 1 < g.nx; ++i) {
      const std::uint32_t a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
      m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
    }
  }
  // Zero contour by marching squares (segments, ambiguous cells split
  // arbitrarily: it is a picture).
  auto value = [&](std::uint32_t i, std::uint32_t j) { return s.at(i, j, k); };
  auto cross = [&](std::uint32_t i0, std::uint32_t j0, std::uint32_t i1, std::uint32_t j1) {
    const float a = value(i0, j0), b = value(i1, j1);
    const float t = a == b ? 0.5f : a / (a - b);
    const fvec3 p = m.positions[at(i0, j0)], q = m.positions[at(i1, j1)];
    return fvec3{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t + 0.001f};
  };
  for (std::uint32_t j = 0; j + 1 < g.ny; ++j) {
    for (std::uint32_t i = 0; i + 1 < g.nx; ++i) {
      std::vector<fvec3> hits;
      const std::array<std::array<std::uint32_t, 4>, 4> sides{
          {{i, j, i + 1, j}, {i + 1, j, i + 1, j + 1}, {i + 1, j + 1, i, j + 1}, {i, j + 1, i, j}}};
      for (const auto& e : sides) {
        if ((value(e[0], e[1]) < 0) != (value(e[2], e[3]) < 0)) hits.push_back(cross(e[0], e[1], e[2], e[3]));
      }
      for (std::size_t h = 0; h + 1 < hits.size(); h += 2) m.edges.push_back({hits[h], hits[h + 1]});
    }
  }
  return m;
}

}  // namespace

Scene preview_scene(const proc::Preview& preview, const Id& volume, const Id& node, const fbox3& bounds,
                    const NodePreviewOptions& options) {
  Scene scene;
  SceneItem item;
  item.role = ItemRole::NodePreview;
  item.subject = node;
  item.volume = volume;
  item.color = kPreviewColor;
  switch (preview.type) {
    case proc::PortType::Field:
      if (options.field == FieldView::Slice && preview.samples) {
        item.mesh = slice(*preview.samples, options.slice);
        item.color = Rgb{0.2f, 0.2f, 0.2f};  // the contour
      } else if (preview.mesh) {
        item.mesh = display_mesh(*preview.mesh);
        item.transparency = 0.15f;
      }
      break;
    case proc::PortType::Mesh:
      if (preview.mesh) item.mesh = display_mesh(*preview.mesh);
      break;
    case proc::PortType::Children:  // L4: the children's boxes
      if (preview.mesh) {
        item.mesh = display_mesh(*preview.mesh);
        item.transparency = 0.4f;
      }
      break;
    case proc::PortType::Frames:
    case proc::PortType::Curves: {
      const fvec3 d = bounds.is_empty() ? fvec3{10, 10, 10} : bounds.max - bounds.min;
      const float diagonal = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
      item.mesh = glyphs(preview.points, std::max(0.05f, diagonal * 0.012f));
      break;
    }
    default: break;
  }
  if (item.mesh.empty() && item.mesh.edges.empty()) return scene;
  scene.bounds = item.mesh.bounds();
  scene.items.push_back(std::move(item));
  return scene;
}

}  // namespace vs::app

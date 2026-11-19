#include "meshers.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <catch2/catch_test_macros.hpp>

#include "vs/mesh/triangulate.hpp"

namespace meshers {

using vs::core::fvec3;
using vs::core::to_fp32;
using vs::mesh::OutlineLoop;
using vs::mesh::OutlineSegment;
using vs::mesh::Triangle;

namespace {

constexpr double kPi = std::numbers::pi;

double uniform(std::mt19937_64& rng, double lo, double hi) {
  return std::uniform_real_distribution<double>(lo, hi)(rng);
}

// One end's outline divided into chains: per loop, per segment, the mesh
// vertices from the segment's start to its end (the next one's start).
using Chains = std::vector<std::vector<std::vector<std::uint32_t>>>;

struct EndMesh {
  Chains chains;
};

EndMesh add_end(Mesh& mesh, const End& end, bool is_top, const Id& volume) {
  EndMesh out;
  if (end.seam != nullptr) {
    const Seam& seam = *end.seam;
    const std::vector<std::uint32_t> map = vs::mesh::add_seam_vertices(mesh, seam);
    vs::mesh::add_seam_face(mesh, seam, map, is_top ? seam.normal() : -seam.normal(), volume);
    for (const vs::mesh::SeamLoop& loop : seam.loops) {
      std::vector<std::vector<std::uint32_t>> segs;
      for (std::size_t i = 0; i < loop.segment_starts.size(); ++i) {
        const std::uint32_t from = loop.segment_starts[i];
        const std::uint32_t to = i + 1 < loop.segment_starts.size()
                                     ? loop.segment_starts[i + 1]
                                     : static_cast<std::uint32_t>(loop.vertices.size());
        std::vector<std::uint32_t> chain;
        for (std::uint32_t k = from; k <= to; ++k) {
          chain.push_back(map[loop.vertices[k % loop.vertices.size()]]);
        }
        segs.push_back(std::move(chain));
      }
      out.chains.push_back(std::move(segs));
    }
    return out;
  }
  const SeamOutline& outline = *end.outline;
  std::vector<dvec2> pts;
  std::vector<vs::mesh::Loop> loops;
  std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> ranges;  // per loop, per segment
  for (const OutlineLoop& loop : outline.loops) {
    vs::mesh::Loop il;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> r;
    for (const OutlineSegment& s : loop.segments) {
      double n = std::max(1.0, std::ceil(s.length() / end.spacing));
      if (s.kind == OutlineSegment::Kind::Arc) n = std::max(n, std::ceil(std::abs(s.sweep) / kPi * 4));
      const auto count = static_cast<std::uint32_t>(n);
      r.emplace_back(static_cast<std::uint32_t>(il.size()), count);
      for (std::uint32_t k = 0; k < count; ++k) {
        il.push_back(static_cast<std::uint32_t>(pts.size()));
        pts.push_back(s.point(static_cast<double>(k) / static_cast<double>(count)));
      }
    }
    loops.push_back(std::move(il));
    ranges.push_back(std::move(r));
  }
  const auto base = static_cast<std::uint32_t>(mesh.vertex_count());
  for (const dvec2& p : pts) {
    mesh.add_vertex(to_fp32(outline.frame.to_model(p) + end.offset));
  }
  auto tris = vs::mesh::triangulate(pts, loops);
  REQUIRE(tris.ok());
  for (const Triangle& t : *tris) {
    if (is_top) {
      mesh.add_triangle(base + t[0], base + t[1], base + t[2], volume);
    } else {
      mesh.add_triangle(base + t[0], base + t[2], base + t[1], volume);
    }
  }
  for (std::size_t l = 0; l < loops.size(); ++l) {
    std::vector<std::vector<std::uint32_t>> segs;
    for (const auto& [from, count] : ranges[l]) {
      std::vector<std::uint32_t> chain;
      for (std::uint32_t k = 0; k <= count; ++k) {
        chain.push_back(base + loops[l][(from + k) % loops[l].size()]);
      }
      segs.push_back(std::move(chain));
    }
    out.chains.push_back(std::move(segs));
  }
  return out;
}

// Joins two chains running the same way: b on the lower end, t on the upper.
void strip(Mesh& mesh, const std::vector<std::uint32_t>& b, const std::vector<std::uint32_t>& t,
           const Id& volume) {
  const std::size_t nb = b.size() - 1, nt = t.size() - 1;
  std::size_t i = 0, j = 0;
  while (i < nb || j < nt) {
    const bool advance_bottom = j == nt || (i < nb && (i + 1) * nt <= (j + 1) * nb);
    if (advance_bottom) {
      mesh.add_triangle(b[i], b[i + 1], t[j], volume);
      ++i;
    } else {
      mesh.add_triangle(b[i], t[j + 1], t[j], volume);
      ++j;
    }
  }
}

}  // namespace

vs::sketch::SketchPlane random_frame(std::mt19937_64& rng, double reach) {
  std::normal_distribution<double> g(0.0, 1.0);
  dvec3 n{g(rng), g(rng), g(rng)};
  while (vs::core::length(n) < 1e-3) n = {g(rng), g(rng), g(rng)};
  n = vs::core::normalize(n);
  const dvec3 any = std::abs(n.x) < 0.9 ? dvec3{1, 0, 0} : dvec3{0, 1, 0};
  const dvec3 x = vs::core::normalize(vs::core::cross(n, any));
  const dvec3 y = vs::core::cross(n, x);
  return {{uniform(rng, -reach, reach), uniform(rng, -reach, reach), uniform(rng, -reach, reach)},
          x,
          y};
}

SeamOutline random_outline(std::mt19937_64& rng, const vs::sketch::SketchPlane& frame,
                           double size, int max_holes) {
  SeamOutline out;
  out.frame = frame;
  const int corners = std::uniform_int_distribution<int>(12, 40)(rng);
  const double phase = uniform(rng, 0, 2 * kPi);
  std::vector<dvec2> pts;
  for (int k = 0; k < corners; ++k) {
    const double step = 2 * kPi / corners;
    const double a = phase + step * (k + uniform(rng, -0.4, 0.4));
    const double r = size * uniform(rng, 0.6, 1.0);
    pts.push_back({r * std::cos(a), r * std::sin(a)});
  }
  OutlineLoop outer;
  for (std::size_t k = 0; k < pts.size(); ++k) {
    outer.segments.push_back(OutlineSegment::line(pts[k], pts[(k + 1) % pts.size()]));
  }
  out.loops.push_back(std::move(outer));

  const int holes = std::uniform_int_distribution<int>(0, max_holes)(rng);
  std::vector<std::pair<dvec2, double>> placed;
  for (int h = 0, tries = 0; h < holes && tries < 200; ++tries) {
    const double rh = size * uniform(rng, 0.03, 0.12);
    const double reach = 0.45 * size - rh;
    const dvec2 c{uniform(rng, -reach, reach), uniform(rng, -reach, reach)};
    if (vs::core::length(c) > reach) continue;
    bool clear = true;
    for (const auto& [pc, pr] : placed) {
      if (vs::core::length(c - pc) < rh + pr + 0.04 * size) clear = false;
    }
    if (!clear) continue;
    placed.emplace_back(c, rh);
    OutlineLoop hole;
    if (std::uniform_int_distribution<int>(0, 1)(rng) == 0) {
      hole.segments.push_back(OutlineSegment::arc(c, rh, uniform(rng, 0, 2 * kPi), -2 * kPi));
    } else {
      const int n = std::uniform_int_distribution<int>(3, 8)(rng);
      const double a0 = uniform(rng, 0, 2 * kPi);
      std::vector<dvec2> hp;
      for (int k = 0; k < n; ++k) {
        const double a = a0 - 2 * kPi * k / n;  // clockwise
        hp.push_back({c.x + rh * std::cos(a), c.y + rh * std::sin(a)});
      }
      for (int k = 0; k < n; ++k) {
        hole.segments.push_back(OutlineSegment::line(hp[static_cast<std::size_t>(k)],
                                                     hp[static_cast<std::size_t>((k + 1) % n)]));
      }
    }
    out.loops.push_back(std::move(hole));
    ++h;
  }
  return out;
}

SeamOutline rectangle(const vs::sketch::SketchPlane& frame, dvec2 c, double w, double h) {
  SeamOutline out;
  out.frame = frame;
  const dvec2 p[4] = {{c.x - w / 2, c.y - h / 2},
                      {c.x + w / 2, c.y - h / 2},
                      {c.x + w / 2, c.y + h / 2},
                      {c.x - w / 2, c.y + h / 2}};
  OutlineLoop loop;
  for (int k = 0; k < 4; ++k) loop.segments.push_back(OutlineSegment::line(p[k], p[(k + 1) % 4]));
  out.loops.push_back(std::move(loop));
  return out;
}

Mesh prism(const End& bottom, const End& top, const Id& volume) {
  Mesh mesh;
  const EndMesh lo = add_end(mesh, bottom, false, volume);
  const EndMesh hi = add_end(mesh, top, true, volume);
  REQUIRE(lo.chains.size() == hi.chains.size());
  for (std::size_t l = 0; l < lo.chains.size(); ++l) {
    REQUIRE(lo.chains[l].size() == hi.chains[l].size());
    for (std::size_t s = 0; s < lo.chains[l].size(); ++s) {
      strip(mesh, lo.chains[l][s], hi.chains[l][s], volume);
    }
  }
  return mesh;
}

Mesh pyramid(const Seam& seam, double height, int side, const Id& volume) {
  Mesh mesh;
  const std::vector<std::uint32_t> map = vs::mesh::add_seam_vertices(mesh, seam);
  const dvec3 n = seam.normal();
  vs::mesh::add_seam_face(mesh, seam, map, side > 0 ? -n : n, volume);
  const std::uint32_t apex =
      mesh.add_vertex(to_fp32(seam.frame.origin + n * (height * side)));
  const auto& loop = seam.loops.front().vertices;
  for (std::size_t i = 0; i < loop.size(); ++i) {
    const std::uint32_t a = map[loop[i]], b = map[loop[(i + 1) % loop.size()]];
    if (side > 0) {
      mesh.add_triangle(a, b, apex, volume);
    } else {
      mesh.add_triangle(b, a, apex, volume);
    }
  }
  return mesh;
}

Mesh box_under(const Seam& seam, double margin, double depth, const Id& volume) {
  Mesh mesh;
  const std::vector<std::uint32_t> map = vs::mesh::add_seam_vertices(mesh, seam);
  const dvec3 n = seam.normal();
  vs::mesh::add_seam_face(mesh, seam, map, n, volume);
  const auto& outer = seam.loops.front().vertices;
  dvec2 lo = seam.vertices[outer[0]].uv, hi = lo;
  for (const std::uint32_t v : outer) {
    lo = vs::core::min(lo, seam.vertices[v].uv);
    hi = vs::core::max(hi, seam.vertices[v].uv);
  }
  lo = lo - dvec2{margin, margin};
  hi = hi + dvec2{margin, margin};
  const dvec2 rect[4] = {lo, {hi.x, lo.y}, hi, {lo.x, hi.y}};
  // The ring: the rectangle with the seam's outline as a hole.
  std::vector<dvec2> pts(rect, rect + 4);
  vs::mesh::Loop rect_loop{0, 1, 2, 3}, hole;
  std::vector<std::uint32_t> local_to_mesh;
  std::uint32_t top[4], bottom[4];
  for (int k = 0; k < 4; ++k) {
    top[k] = mesh.add_vertex(to_fp32(seam.frame.to_model(rect[k])));
    local_to_mesh.push_back(top[k]);
  }
  for (const std::uint32_t v : outer) {
    hole.push_back(static_cast<std::uint32_t>(pts.size()));
    pts.push_back(seam.vertices[v].uv);
    local_to_mesh.push_back(map[v]);
  }
  auto ring = vs::mesh::triangulate(pts, {rect_loop, hole});
  REQUIRE(ring.ok());
  for (const Triangle& t : *ring) {
    mesh.add_triangle(local_to_mesh[t[0]], local_to_mesh[t[1]], local_to_mesh[t[2]], volume);
  }
  for (int k = 0; k < 4; ++k) {
    bottom[k] = mesh.add_vertex(to_fp32(seam.frame.to_model(rect[k]) - n * depth));
  }
  for (int k = 0; k < 4; ++k) {
    const int k1 = (k + 1) % 4;
    mesh.add_triangle(bottom[k], bottom[k1], top[k1], volume);
    mesh.add_triangle(bottom[k], top[k1], top[k], volume);
  }
  mesh.add_triangle(bottom[0], bottom[2], bottom[1], volume);
  mesh.add_triangle(bottom[0], bottom[3], bottom[2], volume);
  return mesh;
}

}  // namespace meshers

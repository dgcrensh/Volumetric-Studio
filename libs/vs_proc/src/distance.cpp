#include "vs/proc/distance.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace vs::proc {

namespace {

constexpr std::uint32_t kLeafSize = 4;

double box_distance_squared(const dbox3& b, const dvec3& p) {
  double d = 0.0;
  const double lo[3] = {b.min.x, b.min.y, b.min.z};
  const double hi[3] = {b.max.x, b.max.y, b.max.z};
  const double q[3] = {p.x, p.y, p.z};
  for (int i = 0; i < 3; ++i) {
    if (q[i] < lo[i]) {
      d += (lo[i] - q[i]) * (lo[i] - q[i]);
    } else if (q[i] > hi[i]) {
      d += (q[i] - hi[i]) * (q[i] - hi[i]);
    }
  }
  return d;
}

double axis_of(const dvec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

// The point of triangle abc nearest p, and which feature it lies on
// (Ericson, Real-Time Collision Detection, 5.1.5).
std::pair<dvec3, int> closest_on_triangle(const dvec3& p, const dvec3& a, const dvec3& b,
                                          const dvec3& c) {
  const dvec3 ab = b - a;
  const dvec3 ac = c - a;
  const dvec3 ap = p - a;
  const double d1 = dot(ab, ap);
  const double d2 = dot(ac, ap);
  if (d1 <= 0.0 && d2 <= 0.0) return {a, 4};
  const dvec3 bp = p - b;
  const double d3 = dot(ab, bp);
  const double d4 = dot(ac, bp);
  if (d3 >= 0.0 && d4 <= d3) return {b, 5};
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
    const double v = d1 / (d1 - d3);
    return {a + ab * v, 1};
  }
  const dvec3 cp = p - c;
  const double d5 = dot(ab, cp);
  const double d6 = dot(ac, cp);
  if (d6 >= 0.0 && d5 <= d6) return {c, 6};
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
    const double w = d2 / (d2 - d6);
    return {a + ac * w, 3};
  }
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
    const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return {b + (c - b) * w, 2};
  }
  const double denom = 1.0 / (va + vb + vc);
  const double v = vb * denom;
  const double w = vc * denom;
  return {a + ab * v + ac * w, 0};
}

// The angle at `at` between the directions to `p` and `q`.
double corner_angle(const dvec3& at, const dvec3& p, const dvec3& q) {
  const dvec3 u = normalize(p - at);
  const dvec3 v = normalize(q - at);
  return std::atan2(length(cross(u, v)), dot(u, v));
}

}  // namespace

TriangleDistance::TriangleDistance(const mesh::Mesh& m, bool closed) : closed_(closed) {
  core::Hasher h;
  h.add(std::string_view("vstudio.proc.triangle_distance"));
  h.add(closed);
  h.add(static_cast<std::uint64_t>(m.positions.size()));
  positions_.reserve(m.positions.size());
  for (const auto& p : m.positions) {
    h.add(p);
    positions_.push_back(core::to_fp64(p));
    bounds_.expand(positions_.back());
  }
  h.add(static_cast<std::uint64_t>(m.indices.size()));
  for (const auto i : m.indices) h.add(i);
  hash_ = h.finalize();

  // Triangles with an area, and each one's face normal.
  for (std::size_t t = 0; t < m.triangle_count(); ++t) {
    const auto tri = m.triangle(t);
    const dvec3 n = cross(positions_[tri[1]] - positions_[tri[0]], positions_[tri[2]] - positions_[tri[0]]);
    const double len = length(n);
    if (!(len > 0.0)) continue;
    Tri out;
    out.v[0] = tri[0];
    out.v[1] = tri[1];
    out.v[2] = tri[2];
    out.normal = n / len;
    triangles_.push_back(out);
  }

  // Pseudonormals: edges sum their faces' normals; vertices weight them by
  // the face's angle at the vertex.
  vertex_normals_.assign(positions_.size(), dvec3{});
  std::map<std::pair<std::uint32_t, std::uint32_t>, dvec3> edges;
  for (const auto& t : triangles_) {
    for (int e = 0; e < 3; ++e) {
      const std::uint32_t a = t.v[e];
      const std::uint32_t b = t.v[(e + 1) % 3];
      const std::uint32_t c = t.v[(e + 2) % 3];
      edges[{std::min(a, b), std::max(a, b)}] += t.normal;
      vertex_normals_[a] += t.normal * corner_angle(positions_[a], positions_[b], positions_[c]);
    }
  }
  for (auto& t : triangles_) {
    for (int e = 0; e < 3; ++e) {
      const std::uint32_t a = t.v[e];
      const std::uint32_t b = t.v[(e + 1) % 3];
      t.edge_normal[e] = edges[{std::min(a, b), std::max(a, b)}];
    }
  }

  if (triangles_.empty()) return;
  order_.resize(triangles_.size());
  for (std::uint32_t i = 0; i < order_.size(); ++i) order_[i] = i;
  nodes_.reserve(2 * triangles_.size() / kLeafSize + 2);
  nodes_.push_back({});
  build(0, 0, static_cast<std::uint32_t>(triangles_.size()), 0);
}

void TriangleDistance::build(std::uint32_t node, std::uint32_t first, std::uint32_t count,
                             int depth) {
  dbox3 box;
  dbox3 centres;
  for (std::uint32_t i = first; i < first + count; ++i) {
    const Tri& t = triangles_[order_[i]];
    dvec3 c{};
    for (const auto v : t.v) {
      box.expand(positions_[v]);
      c += positions_[v];
    }
    centres.expand(c / 3.0);
  }
  nodes_[node].box = box;
  if (count <= kLeafSize || depth > 48) {
    nodes_[node].first = first;
    nodes_[node].count = count;
    return;
  }
  const dvec3 extent = centres.size();
  const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
  const std::uint32_t half = count / 2;
  const auto centre_of = [&](std::uint32_t t) {
    const Tri& tri = triangles_[t];
    return axis_of(positions_[tri.v[0]], axis) + axis_of(positions_[tri.v[1]], axis) +
           axis_of(positions_[tri.v[2]], axis);
  };
  const auto begin = order_.begin() + first;
  std::nth_element(begin, begin + half, begin + count, [&](std::uint32_t a, std::uint32_t b) {
    const double ca = centre_of(a);
    const double cb = centre_of(b);
    return ca < cb || (ca == cb && a < b);
  });
  const auto left = static_cast<std::uint32_t>(nodes_.size());
  nodes_[node].left = left;
  nodes_[node].count = 0;
  nodes_.push_back({});
  nodes_.push_back({});
  build(left, first, half, depth + 1);
  build(left + 1, first + half, count - half, depth + 1);
}

void TriangleDistance::search(std::uint32_t node, const dvec3& p, Nearest& best) const {
  const Node& n = nodes_[node];
  if (n.count > 0) {
    for (std::uint32_t i = n.first; i < n.first + n.count; ++i) {
      const Tri& t = triangles_[order_[i]];
      const auto [q, feature] =
          closest_on_triangle(p, positions_[t.v[0]], positions_[t.v[1]], positions_[t.v[2]]);
      const double d = length_squared(p - q);
      if (d < best.distance_squared) {
        best.distance_squared = d;
        best.point = q;
        best.triangle = order_[i];
        best.feature = feature;
      }
    }
    return;
  }
  const double dl = box_distance_squared(nodes_[n.left].box, p);
  const double dr = box_distance_squared(nodes_[n.left + 1].box, p);
  const std::uint32_t first = dl <= dr ? n.left : n.left + 1;
  const std::uint32_t second = dl <= dr ? n.left + 1 : n.left;
  if (std::min(dl, dr) < best.distance_squared) search(first, p, best);
  if (std::max(dl, dr) < best.distance_squared) search(second, p, best);
}

TriangleDistance::Nearest TriangleDistance::nearest(const dvec3& p) const {
  Nearest best;
  best.distance_squared = std::numeric_limits<double>::infinity();
  if (!nodes_.empty()) search(0, p, best);
  return best;
}

double TriangleDistance::distance(const dvec3& p) const {
  return std::sqrt(nearest(p).distance_squared);
}

double TriangleDistance::signed_distance(const dvec3& p) const {
  const Nearest n = nearest(p);
  const double d = std::sqrt(n.distance_squared);
  if (!closed_ || triangles_.empty()) return d;
  const Tri& t = triangles_[n.triangle];
  dvec3 normal;
  if (n.feature == 0) {
    normal = t.normal;
  } else if (n.feature <= 3) {
    normal = t.edge_normal[n.feature - 1];
  } else {
    normal = vertex_normals_[t.v[n.feature - 4]];
  }
  return dot(p - n.point, normal) < 0.0 ? -d : d;
}

}  // namespace vs::proc

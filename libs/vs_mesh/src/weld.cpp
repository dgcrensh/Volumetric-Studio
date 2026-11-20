#include "vs/mesh/weld.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

namespace vs::mesh {
namespace {

using core::Diagnostic;
using core::Id;
using core::SubjectRef;

constexpr std::uint32_t kNoSeam = 0xffffffffu;

std::array<std::uint32_t, 3> sorted(Triangle t) {
  std::sort(t.begin(), t.end());
  return t;
}

// Collects one diagnostic per (code, subjects), counting repeats.
class Reporter {
 public:
  void add(const std::string& code, const std::string& what, std::vector<SubjectRef> subjects) {
    std::string key = code;
    for (const SubjectRef& s : subjects) key += "|" + s.id.to_string();
    auto [it, added] = index_.try_emplace(key, entries_.size());
    if (added) entries_.push_back({code, what, std::move(subjects), 0});
    ++entries_[it->second].count;
  }
  void flush(core::Diagnostics& out) {
    for (const Entry& e : entries_) {
      std::string message = e.what;
      if (e.count > 1) message += " (" + std::to_string(e.count) + " places)";
      out.add(Diagnostic::error(e.code, message, e.subjects));
    }
    entries_.clear();
    index_.clear();
  }

 private:
  struct Entry {
    std::string code;
    std::string what;
    std::vector<SubjectRef> subjects;
    std::size_t count = 0;
  };
  std::vector<Entry> entries_;
  std::map<std::string, std::size_t> index_;
};

}  // namespace

WeldResult weld(std::span<const Mesh> meshes, const SeamLibrary& lib) {
  WeldResult r;
  Reporter report;

  // 1. Each mesh on its own.
  for (const Mesh& m : meshes) {
    const std::vector<Id> vols = m.volumes();
    const SubjectRef subject = vols.empty() ? SubjectRef{} : lib.volume_subject(vols.front());
    r.diagnostics.append(m.check(subject));
  }
  if (r.diagnostics.has_errors()) return r;

  // 2. Concatenate, merging seam vertices by id.
  Mesh& out = r.mesh;
  std::unordered_map<Id, std::uint32_t> merged;
  std::vector<std::uint32_t> seam_of;  // per output vertex: the seam it is on, or kNoSeam
  std::vector<std::uint32_t> seam_index;  // per output vertex: its index in that seam
  for (const Mesh& m : meshes) {
    // The first volume using each vertex, to name it.
    std::vector<Id> owner(m.vertex_count());
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
      for (const std::uint32_t v : m.triangle(t)) {
        if (owner[v].is_null()) owner[v] = m.triangle_volumes[t];
      }
    }
    std::vector<std::uint32_t> map(m.vertex_count());
    for (std::uint32_t v = 0; v < m.vertex_count(); ++v) {
      const Id& sid = m.seam_vertices[v];
      if (sid.is_null()) {
        map[v] = out.add_vertex(m.positions[v]);
        seam_of.push_back(kNoSeam);
        seam_index.push_back(0);
        continue;
      }
      const std::optional<SeamVertexRef> ref = lib.find_vertex(sid);
      if (!ref) {
        report.add("mesh.weld.unknown_vertex", "A vertex carries an id that is no seam's",
                   {lib.volume_subject(owner[v])});
        map[v] = out.add_vertex(m.positions[v]);
        seam_of.push_back(kNoSeam);
        seam_index.push_back(0);
        continue;
      }
      const Seam& seam = lib.seams[ref->seam];
      if (!(m.positions[v] == seam.vertices[ref->vertex].position32)) {
        report.add("mesh.weld.moved", "A seam vertex isn't where the seam puts it",
                   {seam.subject(), lib.volume_subject(owner[v])});
      }
      const auto [it, added] = merged.try_emplace(sid, static_cast<std::uint32_t>(out.vertex_count()));
      if (added) {
        out.add_vertex(seam.vertices[ref->vertex].position32, sid);
        seam_of.push_back(ref->seam);
        seam_index.push_back(ref->vertex);
      } else {
        ++r.merged_vertices;
      }
      map[v] = it->second;
    }
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
      const Triangle tri = m.triangle(t);
      out.add_triangle(map[tri[0]], map[tri[1]], map[tri[2]], m.triangle_volumes[t]);
    }
  }
  const std::set<Id> present = [&] {
    std::set<Id> s(out.triangle_volumes.begin(), out.triangle_volumes.end());
    return s;
  }();

  // 3. Conformity, per seam and volume.
  // Each seam's triangles, as sorted seam vertex indices.
  std::vector<std::vector<std::array<std::uint32_t, 3>>> expected(lib.seams.size());
  std::vector<std::vector<std::array<std::uint32_t, 3>>> fills(lib.seams.size());
  for (std::uint32_t s = 0; s < lib.seams.size(); ++s) {
    for (const Triangle& t : lib.seams[s].triangles) expected[s].push_back(sorted(t));
    std::sort(expected[s].begin(), expected[s].end());
    for (const auto& fill : lib.seams[s].hole_fills) {
      for (const Triangle& t : fill) fills[s].push_back(sorted(t));
    }
    std::sort(fills[s].begin(), fills[s].end());
  }
  // Seam triangles: one of a seam's triangles, all three corners on it.
  // Other triangles may join three seam vertices too (a ring round a
  // concave outline, outside the patch); they are the mesher's own. Side:
  // +1 when it winds like the seam's triangulation (counter-clockwise
  // about the normal), -1 when turned, 0 when flat.
  std::vector<std::uint32_t> tri_seam(out.triangle_count(), kNoSeam);
  std::vector<bool> tri_fill(out.triangle_count(), false);  // one of a seam's hole fills
  std::vector<int> tri_side(out.triangle_count(), 0);
  // (seam, volume) -> the seam triangles it has, and which way each faces.
  std::map<std::pair<std::uint32_t, Id>, std::vector<std::pair<std::array<std::uint32_t, 3>, int>>>
      observed;
  for (std::size_t t = 0; t < out.triangle_count(); ++t) {
    const Triangle tri = out.triangle(t);
    const std::uint32_t s = seam_of[tri[0]];
    if (s == kNoSeam || seam_of[tri[1]] != s || seam_of[tri[2]] != s) continue;
    const auto corners = sorted({seam_index[tri[0]], seam_index[tri[1]], seam_index[tri[2]]});
    const bool is_seam = std::binary_search(expected[s].begin(), expected[s].end(), corners);
    const bool is_fill = !is_seam && std::binary_search(fills[s].begin(), fills[s].end(), corners);
    if (!is_seam && !is_fill) continue;
    const Seam& seam = lib.seams[s];
    // In uv, unwrapped across a band's cut (L2).
    const std::array<dvec2, 3> q =
        seam.triangle_uv({seam_index[tri[0]], seam_index[tri[1]], seam_index[tri[2]]});
    const double o = core::cross(q[1] - q[0], q[2] - q[0]);
    tri_side[t] = o > 0 ? 1 : (o < 0 ? -1 : 0);
    if (is_fill) {
      tri_fill[t] = true;
      continue;
    }
    tri_seam[t] = s;
    observed[{s, out.triangle_volumes[t]}].emplace_back(corners, tri_side[t]);
  }
  for (std::uint32_t s = 0; s < lib.seams.size(); ++s) {
    const Seam& seam = lib.seams[s];
    for (const Id& v : present) {
      const bool joined = seam.volumes.empty() ||
                          std::find(seam.volumes.begin(), seam.volumes.end(), v) != seam.volumes.end();
      const auto it = observed.find({s, v});
      if (it == observed.end()) {
        if (joined && !seam.volumes.empty()) {
          report.add("mesh.weld.nonconforming",
                     "The volume's mesh doesn't use the surface's seam",
                     {seam.subject(), lib.volume_subject(v)});
        }
        continue;
      }
      if (!joined) {
        report.add("mesh.weld.not_joined",
                   "The volume meshes the seam of a surface that doesn't join it",
                   {seam.subject(), lib.volume_subject(v)});
        continue;
      }
      std::vector<std::array<std::uint32_t, 3>> got;
      got.reserve(it->second.size());
      int side = 0;
      bool one_way = true;
      for (const auto& [tri, sd] : it->second) {
        got.push_back(tri);
        if (sd == 0 || (side != 0 && sd != side)) one_way = false;
        if (side == 0) side = sd;
      }
      std::sort(got.begin(), got.end());
      if (got != expected[s] || !one_way) {
        std::vector<std::array<std::uint32_t, 3>> diff;
        std::set_difference(expected[s].begin(), expected[s].end(), got.begin(), got.end(),
                            std::back_inserter(diff));
        report.add("mesh.weld.nonconforming",
                   "The volume's mesh of the seam isn't the seam's triangulation (" +
                       std::to_string(diff.size()) + " of " + std::to_string(expected[s].size()) +
                       " seam triangles missing" + (one_way ? "" : ", not all facing one way") +
                       ")",
                   {seam.subject(), lib.volume_subject(v)});
      }
    }
  }

  // 4. Internal faces: a seam triangle (or hole fill triangle) two volumes
  // share, facing opposite ways.
  std::map<std::array<std::uint32_t, 3>, std::vector<std::uint32_t>> by_corners;
  for (std::uint32_t t = 0; t < out.triangle_count(); ++t) {
    if (tri_seam[t] != kNoSeam || tri_fill[t]) by_corners[sorted(out.triangle(t))].push_back(t);
  }
  std::vector<bool> keep(out.triangle_count(), true);
  for (const auto& [corners, ts] : by_corners) {
    if (ts.size() == 2 && out.triangle_volumes[ts[0]] != out.triangle_volumes[ts[1]] &&
        tri_side[ts[0]] != 0 && tri_side[ts[0]] == -tri_side[ts[1]]) {
      keep[ts[0]] = keep[ts[1]] = false;
      r.internal_triangles += 2;
    }
  }

  // 5. Compact: kept triangles, used vertices.
  Mesh compact;
  std::vector<std::uint32_t> remap(out.vertex_count(), kNoSeam);
  std::vector<std::uint32_t> compact_seam;
  for (std::uint32_t t = 0; t < out.triangle_count(); ++t) {
    if (!keep[t]) continue;
    Triangle tri = out.triangle(t);
    for (std::uint32_t& v : tri) {
      if (remap[v] == kNoSeam) {
        remap[v] = compact.add_vertex(out.positions[v], out.seam_vertices[v]);
        compact_seam.push_back(seam_of[v]);
      }
      v = remap[v];
    }
    compact.add_triangle(tri, out.triangle_volumes[t]);
  }
  r.mesh = std::move(compact);
  r.topology = analyse(r.mesh);

  // Name the place: the seam when every corner involved is on one, then
  // the volumes of the triangles there.
  const Mesh& m = r.mesh;
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> edge_tris;
  std::vector<std::vector<std::uint32_t>> vertex_tris;
  const bool any_problem = !r.topology.watertight() || !r.topology.manifold();
  if (any_problem) {
    vertex_tris.resize(m.vertex_count());
    for (std::uint32_t t = 0; t < m.triangle_count(); ++t) {
      const Triangle tri = m.triangle(t);
      for (std::uint32_t k = 0; k < 3; ++k) {
        const std::uint32_t a = std::min(tri[k], tri[(k + 1) % 3]);
        const std::uint32_t b = std::max(tri[k], tri[(k + 1) % 3]);
        edge_tris[(static_cast<std::uint64_t>(a) << 32) | b].push_back(t);
        vertex_tris[tri[k]].push_back(t);
      }
    }
  }
  const auto subjects = [&](std::initializer_list<std::uint32_t> verts,
                            const std::vector<std::uint32_t>& tris) {
    std::vector<SubjectRef> out_subjects;
    std::uint32_t s = compact_seam[*verts.begin()];
    for (const std::uint32_t v : verts) {
      if (compact_seam[v] != s) s = kNoSeam;
    }
    if (s != kNoSeam) out_subjects.push_back(lib.seams[s].subject());
    std::set<Id> vols;
    for (const std::uint32_t t : tris) vols.insert(m.triangle_volumes[t]);
    for (const Id& v : vols) out_subjects.push_back(lib.volume_subject(v));
    return out_subjects;
  };
  const auto edge_subjects = [&](const Edge& e) {
    return subjects({e.a, e.b}, edge_tris[(static_cast<std::uint64_t>(e.a) << 32) | e.b]);
  };
  for (const Edge& e : r.topology.boundary_edges) {
    report.add("mesh.weld.open", "The welded mesh is open along an edge", edge_subjects(e));
  }
  for (const Edge& e : r.topology.non_manifold_edges) {
    report.add("mesh.weld.non_manifold", "More than two triangles share an edge", edge_subjects(e));
  }
  for (const Edge& e : r.topology.inconsistent_edges) {
    report.add("mesh.weld.orientation", "Neighbouring triangles face opposite ways",
               edge_subjects(e));
  }
  for (const std::uint32_t v : r.topology.non_manifold_vertices) {
    report.add("mesh.weld.non_manifold", "Separate surfaces touch at a vertex",
               subjects({v}, vertex_tris[v]));
  }
  for (const std::uint32_t t : r.topology.degenerate_triangles) {
    const Triangle tri = m.triangle(t);
    report.add("mesh.weld.non_manifold", "A triangle repeats a vertex",
               subjects({tri[0], tri[1], tri[2]}, {t}));
  }
  report.flush(r.diagnostics);
  return r;
}

}  // namespace vs::mesh

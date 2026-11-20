#include "vs/mesh/mesh.hpp"

#include <string>
#include <unordered_set>

namespace vs::mesh {

std::uint32_t Mesh::add_vertex(const fvec3& position, const Id& seam_vertex) {
  positions.push_back(position);
  seam_vertices.push_back(seam_vertex);
  return static_cast<std::uint32_t>(positions.size() - 1);
}

void Mesh::add_triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c, const Id& volume) {
  indices.push_back(a);
  indices.push_back(b);
  indices.push_back(c);
  triangle_volumes.push_back(volume);
}

void Mesh::append(const Mesh& other) {
  const auto offset = static_cast<std::uint32_t>(positions.size());
  positions.insert(positions.end(), other.positions.begin(), other.positions.end());
  seam_vertices.insert(seam_vertices.end(), other.seam_vertices.begin(), other.seam_vertices.end());
  indices.reserve(indices.size() + other.indices.size());
  for (const std::uint32_t i : other.indices) indices.push_back(i + offset);
  triangle_volumes.insert(triangle_volumes.end(), other.triangle_volumes.begin(),
                          other.triangle_volumes.end());
}

core::fbox3 Mesh::bounds() const {
  core::fbox3 box;
  for (const fvec3& p : positions) box.expand(p);
  return box;
}

std::vector<Id> Mesh::volumes() const {
  std::vector<Id> out;
  std::unordered_set<Id> seen;
  for (const Id& v : triangle_volumes) {
    if (seen.insert(v).second) out.push_back(v);
  }
  return out;
}

Diagnostics Mesh::check(const core::SubjectRef& subject) const {
  Diagnostics out;
  std::vector<core::SubjectRef> subjects;
  if (!subject.id.is_null() || !subject.label.empty()) subjects.push_back(subject);
  const auto fail = [&](std::string message) {
    out.add(Diagnostic::error("mesh.invalid", std::move(message), subjects));
  };
  if (indices.size() % 3 != 0) fail("Index count is not a multiple of three");
  if (seam_vertices.size() != positions.size()) fail("Seam vertex ids don't match the vertex count");
  if (triangle_volumes.size() != triangle_count()) {
    fail("Triangle volume ids don't match the triangle count");
  }
  for (const std::uint32_t i : indices) {
    if (i >= positions.size()) {
      fail("Triangle index " + std::to_string(i) + " is out of range");
      break;
    }
  }
  return out;
}

validate::MeshView view(const Mesh& mesh) {
  return validate::MeshView{mesh.positions, mesh.indices, {}};
}

double double_area(const Mesh& mesh, std::size_t triangle) {
  const Triangle t = mesh.triangle(triangle);
  const core::dvec3 a = core::to_fp64(mesh.positions[t[0]]);
  const core::dvec3 b = core::to_fp64(mesh.positions[t[1]]);
  const core::dvec3 c = core::to_fp64(mesh.positions[t[2]]);
  return core::length(core::cross(b - a, c - a));
}

double enclosed_volume(const Mesh& mesh) {
  double six_v = 0.0;
  for (std::size_t t = 0; t < mesh.triangle_count(); ++t) {
    const Triangle tri = mesh.triangle(t);
    const core::dvec3 a = core::to_fp64(mesh.positions[tri[0]]);
    const core::dvec3 b = core::to_fp64(mesh.positions[tri[1]]);
    const core::dvec3 c = core::to_fp64(mesh.positions[tri[2]]);
    six_v += core::dot(a, core::cross(b, c));
  }
  return six_v / 6.0;
}

}  // namespace vs::mesh

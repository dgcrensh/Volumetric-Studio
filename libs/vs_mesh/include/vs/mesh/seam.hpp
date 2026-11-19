#pragma once

// Seams (spec, "Seam conformity"): how a planar connection surface's
// outline is divided into edges and vertices, and how its interior is
// triangulated, once, for every volume that touches it.
//
// A Seam is made from the surface's exact outline (SeamOutline: lines and
// circular arcs in the surface's plane frame) and a spacing:
//
//   - each outline segment is divided into n equal pieces (equal length
//     along an arc), n = ceil(length / spacing), at least 1 for a line and
//     at least 16 per full turn for an arc. The vertices are the segments'
//     division points, loop by loop;
//   - the interior gets Steiner points on a triangular lattice of the same
//     spacing, aligned to the frame's origin, kept at least half a spacing
//     from the outline;
//   - outline and interior points are triangulated together, constrained
//     Delaunay (triangulate.hpp).
//
// Everything is FP64 and a pure function of (surface id, outline,
// spacing): two volumes that ask for the seam get the same vertices, ids
// and triangles, bit for bit, on the same platform. Each vertex has an id,
// Id::derive(base, index), where `base` comes from a hash of the inputs,
// so a mesh made against an older seam can't pass for one made against
// this one. FP32 positions (position32) are what meshes must use, so the
// two sides of a seam share the same floats.
//
// The spacing is set per surface: ConnectionSurface::seam.spacing when
// positive, otherwise derive_spacing(). The seam library (seam_library.hpp)
// makes one Seam per surface; meshers use it through add_seam_vertices()
// and add_seam_face() rather than making their own.
//
// Orientation: the outer loop runs counter-clockwise about the frame's
// normal and holes clockwise, whatever order the outline came in;
// `triangles` wind counter-clockwise about the normal. A volume on the
// normal's side of the surface has the seam face with its outward normal
// against the frame's normal (triangles_facing() turns them for it).
//
// Codes (the surface first):
//   mesh.seam.spacing           a spacing that isn't positive and finite, or
//                               below the minimum feature size
//   mesh.seam.too_fine          the spacing would need more than
//                               kMaxSeamVertices vertices
//   mesh.seam.outline           no outer loop, or an empty loop
//   mesh.triangulate.*          passed through (triangulate.hpp)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/hash.hpp"
#include "vs/core/id.hpp"
#include "vs/core/settings.hpp"
#include "vs/core/vec.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/mesh/triangulate.hpp"
#include "vs/sketch/sketch.hpp"

namespace vs::mesh {

using core::dvec2;
using core::dvec3;

// One piece of a seam outline, in the frame's 2D coordinates (mm).
struct OutlineSegment {
  enum class Kind { Line, Arc };
  Kind kind = Kind::Line;
  dvec2 start{};
  dvec2 end{};
  // Arc only: points centre + radius·(cos θ, sin θ), θ from start_angle
  // through start_angle + sweep (negative sweep: clockwise).
  dvec2 centre{};
  double radius = 0.0;
  double start_angle = 0.0;
  double sweep = 0.0;

  static OutlineSegment line(const dvec2& a, const dvec2& b);
  static OutlineSegment arc(const dvec2& centre, double radius, double start_angle, double sweep);
  // A full circle, counter-clockwise from angle 0.
  static OutlineSegment circle(const dvec2& centre, double radius);

  [[nodiscard]] double length() const;
  // The point a fraction t ∈ [0, 1] of the way along (by length).
  [[nodiscard]] dvec2 point(double t) const;
  // The same segment run the other way.
  [[nodiscard]] OutlineSegment reversed() const;

  friend bool operator==(const OutlineSegment&, const OutlineSegment&) = default;
};

// A closed loop: each segment starts where the previous one ends.
struct OutlineLoop {
  std::vector<OutlineSegment> segments;
  friend bool operator==(const OutlineLoop&, const OutlineLoop&) = default;
};

// A planar surface's exact outline: loops[0] is the outer boundary, the
// rest are holes, all in `frame`'s 2D coordinates.
struct SeamOutline {
  sketch::SketchPlane frame;
  std::vector<OutlineLoop> loops;
  friend bool operator==(const SeamOutline&, const SeamOutline&) = default;
};

struct SeamVertex {
  core::Id id;
  dvec2 uv{};           // in the frame
  dvec3 position{};     // FP64, frame.to_model(uv)
  core::fvec3 position32{};  // to_fp32(position): what meshes use
};

struct SeamLoop {
  std::vector<std::uint32_t> vertices;  // seam vertex indices, in order round the loop
  // Where each outline segment begins: positions in `vertices`, ascending.
  std::vector<std::uint32_t> segment_starts;
};

// The most vertices one seam may have.
inline constexpr std::size_t kMaxSeamVertices = 2'000'000;

struct Seam {
  core::Id surface;
  std::string name;              // the surface's name, for messages
  std::vector<core::Id> volumes; // the volumes it joins (filled by the library)
  sketch::SketchPlane frame;
  // The exact outline, oriented as `loops` are; loops[k].segments[i]
  // starts at loops[k].vertices[loops[k].segment_starts[i]].
  SeamOutline outline;
  double spacing = 0.0;
  core::Hash256 key;             // hash of the inputs; vertex ids derive from it
  std::vector<SeamVertex> vertices;  // outline vertices loop by loop, then interior ones
  std::size_t outline_vertex_count = 0;
  std::vector<SeamLoop> loops;   // [0] outer (counter-clockwise), then holes (clockwise)
  std::vector<Triangle> triangles;  // counter-clockwise about the normal
  // hole_fills[h - 1] covers hole loops[h] with triangles over its own
  // vertices only (counter-clockwise about the normal): what a volume
  // whose face has no hole there uses to close it (M8). Two volumes that
  // both fill a hole fill it identically, so the weld removes the pair as
  // it does the seam's own triangles.
  std::vector<std::vector<Triangle>> hole_fills;

  [[nodiscard]] dvec3 normal() const { return frame.normal(); }
  // The index of the vertex with this id, if it is one of this seam's.
  [[nodiscard]] std::optional<std::uint32_t> find(const core::Id& vertex) const;
  // `triangles`, wound counter-clockwise about `normal` (turned when
  // `normal` points against the frame's normal).
  [[nodiscard]] std::vector<Triangle> triangles_facing(const dvec3& normal) const;
  // The triangulated area, mm² (the outline's, with arcs as chords).
  [[nodiscard]] double area() const;
  // A subject naming the surface, for diagnostics.
  [[nodiscard]] core::SubjectRef subject() const;

  // Rebuilds the id lookup; make_seam() does this.
  void index();

 private:
  std::unordered_map<core::Id, std::uint32_t> by_id_;
};

// A spacing for a surface that doesn't set one: a sixteenth of the outer
// loop's bounding-box diagonal, and at least the minimum feature size.
double derive_spacing(const SeamOutline& outline, const core::ProductSettings& settings);

// Divides and triangulates the outline. `surface` names the connection
// surface (kind ConnectionSurface, its id and name).
core::Result<Seam> make_seam(const core::SubjectRef& surface, const SeamOutline& outline,
                             double spacing, const core::ProductSettings& settings);

// For meshers. add_seam_vertices() adds every seam vertex to the mesh (at
// position32, tagged with its id) and returns each one's mesh index;
// add_seam_face() then adds the seam's triangles as a face of `volume`
// whose outward normal is `outward`.
std::vector<std::uint32_t> add_seam_vertices(Mesh& mesh, const Seam& seam);
void add_seam_face(Mesh& mesh, const Seam& seam, const std::vector<std::uint32_t>& vertex_map,
                   const dvec3& outward, const core::Id& volume);

// For mesh files made against the seams (a baked procedural volume, M12):
// files keep positions but not seam ids, so every vertex of `mesh` without
// a seam id whose position is bit for bit some seam vertex's position32
// takes that vertex's id (the first seam's, in order, when several share a
// position). Exact equality only, never a distance: a mesh that wasn't
// made against the seams gains nothing. Returns how many vertices it named.
std::size_t attach_seam_vertices(Mesh& mesh, std::span<const Seam* const> seams);

void hash_append(core::Hasher& h, const OutlineSegment& s);

}  // namespace vs::mesh

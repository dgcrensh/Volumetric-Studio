#pragma once

// What the viewport draws, as plain data (no Qt, no OCCT).
//
// build_scene() turns an evaluation (vs_eval) into display items, so the
// expensive part (tessellating B-reps, computing normals) runs on the
// session's worker thread and the UI thread only uploads triangles. Per
// volume, in document order:
//
//   - its boundary, tessellated, drawn as a transparent tint of the volume's
//     colour with its edges;
//   - its contents, when it shows any: a B-rep (parametric volumes, imported
//     STEP and IGES files) tessellated with its edges, or a mesh (imported
//     mesh files, procedural contents) as it is. Contents shown stale (the
//     volume failed, or wasn't regenerated) are drawn grey;
//
// and per connection surface its patch, coloured by its type (bonded,
// clearance, bolted flange) and drawn just in front of the boundary faces
// it lies on.
//
// Display meshes are FP32, indexed, with one normal per vertex: vertices
// where the surface creases by more than `crease_angle` are split, so flat
// faces stay flat and curved ones smooth.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "vs/core/box.hpp"
#include "vs/core/id.hpp"
#include "vs/core/settings.hpp"
#include "vs/core/vec.hpp"
#include "vs/eval/engine.hpp"
#include "vs/kernel/shape.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/model/document.hpp"

namespace vs::app {

using core::fbox3;
using core::fvec3;
using core::Id;

struct Rgb {
  float r = 0, g = 0, b = 0;
  friend bool operator==(const Rgb&, const Rgb&) = default;
};

struct DisplayMesh {
  std::vector<fvec3> positions;
  std::vector<fvec3> normals;           // one per position, unit length
  std::vector<std::uint32_t> indices;   // three per triangle, counter-clockwise from outside
  std::vector<std::vector<fvec3>> edges;  // polylines drawn over the surface

  [[nodiscard]] std::size_t triangle_count() const { return indices.size() / 3; }
  [[nodiscard]] bool empty() const { return indices.empty(); }
  [[nodiscard]] fbox3 bounds() const;
};

enum class ItemRole { Boundary, Contents, ConnectionSurface };
std::string_view to_string(ItemRole role);

struct SceneItem {
  ItemRole role = ItemRole::Boundary;
  Id subject;  // the volume (boundary, contents) or the connection surface
  DisplayMesh mesh;
  Rgb color;
  float transparency = 0.0f;  // 0 opaque .. 1 invisible
  bool stale = false;         // contents shown stale (drawn grey)
};

struct Scene {
  std::vector<SceneItem> items;
  fbox3 bounds;
  // Why something couldn't be drawn (kernel.tessellate.*), for the log.
  core::Diagnostics diagnostics;

  [[nodiscard]] const SceneItem* find(const Id& subject, ItemRole role) const;
  [[nodiscard]] std::size_t count(ItemRole role) const;
};

struct SceneOptions {
  // Display tessellation: coarser than export, finer than a sketch.
  double deflection = 0.05;  // mm
  double angle = 0.35;       // radians
  double crease_angle = 0.6; // radians: vertices split where faces meet more sharply
};

// The colour volume `index` (in document order) is tinted with.
Rgb volume_color(std::size_t index);
// The colour of a connection surface type.
Rgb connection_color(model::ConnectionType type);
// Stale contents.
inline constexpr Rgb kStaleColor{0.55f, 0.55f, 0.55f};

// A kernel shape (solid or face) as a display mesh with its edges.
core::Result<DisplayMesh> display_mesh(const kernel::Shape& shape, const core::ProductSettings& settings,
                                       const SceneOptions& options = {});
// A generated or imported mesh as a display mesh (no edges).
DisplayMesh display_mesh(const mesh::Mesh& mesh, const SceneOptions& options = {});

Scene build_scene(const eval::Evaluation& evaluation, const core::ProductSettings& settings,
                  const SceneOptions& options = {});

}  // namespace vs::app

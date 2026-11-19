#pragma once

// Mesh files (plan, M8): STL and 3MF out, STL, 3MF and OBJ in.
//
// Writing.
//   - STL: binary, one solid, little endian, facet normals from the FP32
//     positions (computed in FP64). Volumes and seams are lost.
//   - 3MF: a 3MF core package (zip, deflated, zero timestamps) with one
//     model part in millimetres. Each ThreeMfObject becomes one mesh
//     object with its name and, as object metadata, the volume's id
//     ("vstudio:volume"); the build places each once, untransformed.
//     Coordinates are written as the shortest decimal that reads back to
//     the same float, so a file read back gives the same mesh, bit for bit.
//
// Reading gives a mesh with no seam vertices and every triangle on
// Id::null(): the caller assigns the volume (assign_volume()).
//   - STL: binary or ASCII. STL repeats each corner, so corners with the
//     same three floats become one vertex (structurally the same point:
//     no distance threshold).
//   - OBJ: "v" and "f" lines (v, v/vt, v//vn, v/vt/vn and negative
//     indices); polygons are split into fans. Everything else is ignored.
//   - 3MF: the model part the package's relationships name (or
//     3D/3dmodel.model), its mesh objects and components, placed by the
//     build items' transforms, scaled from the model's unit to mm.
// Triangles that repeat a vertex are dropped.
//
// Codes: mesh.io.stl, mesh.io.obj, mesh.io.3mf (a file that doesn't read
// as one, with why), mesh.io.format (an unknown format name), mesh.io.empty
// (a mesh with nothing to write, or a file with no triangles).

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/mesh/mesh.hpp"

namespace vs::mesh {

using Bytes = std::vector<std::byte>;

core::Result<Bytes> write_stl(const Mesh& mesh);

struct ThreeMfObject {
  std::string name;
  core::Id volume;           // written as metadata when not null
  const Mesh* mesh = nullptr;
};

core::Result<Bytes> write_3mf(std::span<const ThreeMfObject> objects, std::string_view title);

core::Result<Mesh> read_stl(std::span<const std::byte> file);
core::Result<Mesh> read_obj(std::span<const std::byte> file);
core::Result<Mesh> read_3mf(std::span<const std::byte> file);

// By format name: "stl", "obj" or "3mf" (any case).
core::Result<Mesh> read_mesh(std::span<const std::byte> file, std::string_view format);
// True for the formats read_mesh() reads.
bool is_mesh_format(std::string_view format);

// Puts every triangle on `volume`.
void assign_volume(Mesh& mesh, const core::Id& volume);

// The part of `mesh` on `volume`, with only the vertices it uses.
Mesh extract_volume(const Mesh& mesh, const core::Id& volume);

}  // namespace vs::mesh

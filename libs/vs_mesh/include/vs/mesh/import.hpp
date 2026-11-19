#pragma once

// Importing a mesh file as a static volume (plan, M8): the file becomes a
// blob of the project and a new volume's static contents, and the volume's
// boundary is the mesh's bounding box, as a box between two new datum
// points of the layout ("<name> min", "<name> max").
//
// The file is read first (io.hpp), so a file that doesn't read as a mesh
// is never added. The change goes through model::edit(), so a document it
// would break (a name or id clash) is left as it was.
//
// Codes: mesh.io.* (the file), mesh.import.flat (the mesh's bounding box
// is thinner than the minimum feature size in some direction, so it can't
// be a boundary), model.* (passed through from the edit).

#include <cstddef>
#include <span>
#include <string_view>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/core/settings.hpp"
#include "vs/model/project.hpp"

namespace vs::mesh {

// Adds the volume and returns its id. `format` is "stl", "3mf" or "obj".
core::Result<core::Id> import_mesh_volume(model::Project& project, std::span<const std::byte> file,
                                          std::string_view format, std::string_view name,
                                          const core::ProductSettings& settings);

}  // namespace vs::mesh

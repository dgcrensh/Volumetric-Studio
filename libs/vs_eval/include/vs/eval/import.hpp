#pragma once

// Importing files as static contents (plan, M9: "STEP/IGES import becomes
// static volumes with containment checks").
//
// import_volume() adds a STEP, IGES, STL, 3MF or OBJ file as a new static
// volume whose boundary is the file's bounding box, as a box between two
// new datum points of the layout ("<name> min", "<name> max"); meshes go
// through vs_mesh's import_mesh_volume(). A STEP or IGES file must hold
// exactly one solid (kernel Solid::from(), or a compound of one).
//
// import_contents() fills an existing volume instead: its contents become
// the file, its boundary stays. The engine then checks containment, so a
// file that sticks out of the boundary is reported (validate.containment)
// and the volume fails, keeping its last valid contents.
//
// The file is read first, so a file that doesn't read is never added; the
// change goes through model::edit(), so a document it would break is left
// as it was. Codes: eval.import.format (not a format listed above),
// eval.import.read (a STEP or IGES file that doesn't read as one solid),
// eval.import.flat (too thin for a box boundary), eval.import.volume (no
// such volume, or a keep-out), mesh.io.* and model.* passed through.

#include <cstddef>
#include <span>
#include <string_view>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/core/settings.hpp"
#include "vs/model/project.hpp"

namespace vs::eval {

// The format named by a file extension ("step", "stp", "iges", "igs",
// "stl", "3mf", "obj", any case, with or without the dot), normalised to
// the names StaticContents uses; empty if none.
std::string_view import_format(std::string_view extension);

// Adds a new static volume; returns its id.
core::Result<core::Id> import_volume(model::Project& project, std::span<const std::byte> file,
                                     std::string_view format, std::string_view name,
                                     const core::ProductSettings& settings);

// Makes `volume`'s contents the file.
core::Diagnostics import_contents(model::Project& project, const core::Id& volume,
                                  std::span<const std::byte> file, std::string_view format,
                                  const core::ProductSettings& settings);

}  // namespace vs::eval

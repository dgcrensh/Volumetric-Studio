#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `seams <project>`: loads and builds a .vsproj, makes its seam library
// (vs_mesh, M7) and prints each connection surface's seam: spacing,
// outline and interior vertices, triangles and area. Returns 0 when every
// surface that needs a seam has one, 1 otherwise, 2 for bad arguments.
int run_seams(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

// `export <project> --stl <file> | --3mf <file> [--per-volume] [--placeholders]
// [--deflection <mm>]`: builds the project, tessellates every volume against
// its seams, welds them (vs_mesh, M8) and writes the combined mesh: STL, or
// 3MF as one object (or one per volume with --per-volume). Prints each
// volume and the weld's checks. Writes nothing and returns 1 when the
// combined mesh has errors; 2 for bad arguments.
int run_export(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

// `import <project> <file> [--name <name> | --into <volume>] [--out <project>]`:
// adds a STEP, IGES, STL, 3MF or OBJ file to the project as a static volume
// whose boundary is the file's bounding box (vs_eval import.hpp), or with
// --into makes it the contents of an existing volume, and saves the project
// (in place, or to --out, as a folder or zip like the original).
int run_import(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vs::cli

#include "mesh_commands.hpp"

#include <filesystem>
#include <string>

#include <fmt/format.h>

#include "vs/core/settings.hpp"
#include "vs/mesh/seam_library.hpp"
#include "vs/mesh/topology.hpp"
#include "vs/model/build.hpp"
#include "vs/model/project.hpp"

namespace vs::cli {

int run_seams(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  if (args.size() != 1) {
    err << "usage: vstudio-cli seams <project.vsproj>\n";
    return 2;
  }
  const std::filesystem::path path(std::u8string(args[0].begin(), args[0].end()));
  auto loaded = model::load_project(path);
  if (!loaded) {
    err << loaded.error().to_string() << "\n";
    return 1;
  }
  const auto settings = core::ProductSettings::defaults();
  const model::Document& doc = loaded->document;
  const model::DocumentBuild built = model::build(doc, settings, &loaded->blobs);
  const mesh::SeamLibrary lib = mesh::build_seams(doc, built, settings);

  out << fmt::format("project     {}\n", doc.name);
  out << fmt::format("seams       {} of {} connection surfaces\n", lib.seams.size(),
                     doc.connections.size());
  bool ok = lib.diagnostics.empty();
  for (const model::ConnectionSurface& surface : doc.connections) {
    const mesh::Seam* seam = lib.find(surface.id);
    const std::string name = doc.connection_subject(surface.id).label;
    if (seam == nullptr) {
      const model::ConnectionBuild* cb = built.find_connection(surface.id);
      const char* why = surface.type() == model::ConnectionType::Clearance ? "clearance: no seam"
                        : (cb == nullptr || !cb->ok)                       ? "patch failed to build"
                                                                           : "failed";
      if (surface.type() != model::ConnectionType::Clearance) ok = false;
      out << fmt::format("  {:<12}{}\n", name, why);
      continue;
    }
    std::size_t holes = seam->loops.size() - 1;
    mesh::Mesh face;
    const auto map = mesh::add_seam_vertices(face, *seam);
    mesh::add_seam_face(face, *seam, map, seam->normal(), surface.id);
    const mesh::TopologyReport topo = mesh::analyse(face);
    const bool disc = topo.manifold() && topo.shells == 1 &&
                      topo.euler_characteristic() == 1 - static_cast<long long>(holes);
    ok = ok && disc;
    out << fmt::format(
        "  {:<12}spacing {:.4g} mm ({}), {} outline + {} interior vertices, {} loop{} ({} hole{}), "
        "{} triangles, area {:.6g} mm², {}\n",
        name, seam->spacing, surface.seam.spacing > 0 ? "set" : "derived",
        seam->outline_vertex_count, seam->vertices.size() - seam->outline_vertex_count,
        seam->loops.size(), seam->loops.size() == 1 ? "" : "s", holes, holes == 1 ? "" : "s",
        seam->triangles.size(), seam->area(), disc ? "disc ok" : "NOT A DISC");
  }
  for (const auto& d : lib.diagnostics) out << d.to_string() << "\n";
  out << (ok ? "result      ok\n" : "result      failed\n");
  return ok ? 0 : 1;
}

}  // namespace vs::cli

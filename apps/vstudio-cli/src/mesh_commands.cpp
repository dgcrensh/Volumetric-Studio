#include "mesh_commands.hpp"
#include "files.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "vs/core/settings.hpp"
#include "vs/mesh/combine.hpp"
#include "vs/eval/import.hpp"
#include "vs/mesh/import.hpp"
#include "vs/mesh/io.hpp"
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
  const std::filesystem::path path = to_path(args[0]);
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
    // A band (L2) is a ring: two loops, no holes, Euler characteristic 0.
    const std::size_t holes = seam->band ? 0 : seam->loops.size() - 1;
    mesh::Mesh face;
    const auto map = mesh::add_seam_vertices(face, *seam);
    if (seam->curved()) {
      for (const mesh::Triangle& t : seam->oriented_triangles(true)) {
        face.add_triangle(map[t[0]], map[t[1]], map[t[2]], surface.id);
      }
    } else {
      mesh::add_seam_face(face, *seam, map, seam->normal(), surface.id);
    }
    const mesh::TopologyReport topo = mesh::analyse(face);
    const long long euler = seam->band ? 0 : 1 - static_cast<long long>(holes);
    const bool disc = topo.manifold() && topo.shells == 1 && topo.euler_characteristic() == euler;
    const char* shape = seam->band ? "ring" : "disc";
    ok = ok && disc;
    out << fmt::format(
        "  {:<12}spacing {:.4g} mm ({}), {} outline + {} interior vertices, {} loop{} ({} hole{}), "
        "{} triangles, area {:.6g} mm², {}{}\n",
        name, seam->spacing, surface.seam.spacing > 0 ? "set" : "derived",
        seam->outline_vertex_count, seam->vertices.size() - seam->outline_vertex_count,
        seam->loops.size(), seam->loops.size() == 1 ? "" : "s", holes, holes == 1 ? "" : "s",
        seam->triangles.size(), seam->area(), seam->curved() ? "cylinder, " : "",
        disc ? fmt::format("{} ok", shape) : fmt::format("NOT A {}", seam->band ? "RING" : "DISC"));
  }
  for (const auto& d : lib.diagnostics) out << d.to_string() << "\n";
  out << (ok ? "result      ok\n" : "result      failed\n");
  return ok ? 0 : 1;
}

int run_export(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  constexpr std::string_view kUsage =
      "usage: vstudio-cli export <project.vsproj> --stl <file> | --3mf <file> [--per-volume] "
      "[--placeholders] [--deflection <mm>]\n";
  std::optional<std::string_view> project, stl, three_mf;
  bool per_volume = false;
  mesh::CombineOptions options;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a = args[i];
    const auto value = [&]() -> std::optional<std::string_view> {
      if (i + 1 >= args.size()) return std::nullopt;
      return args[++i];
    };
    if (a == "--stl") {
      stl = value();
      if (!stl) return err << kUsage, 2;
    } else if (a == "--3mf") {
      three_mf = value();
      if (!three_mf) return err << kUsage, 2;
    } else if (a == "--per-volume") {
      per_volume = true;
    } else if (a == "--placeholders") {
      options.placeholders = true;
    } else if (a == "--deflection") {
      const auto v = value();
      double d = 0;
      if (!v || std::from_chars(v->data(), v->data() + v->size(), d).ec != std::errc() || !(d > 0)) {
        return err << kUsage, 2;
      }
      options.tessellation.params.deflection = d;
    } else if (!a.starts_with("-") && !project) {
      project = a;
    } else {
      return err << kUsage, 2;
    }
  }
  if (!project || (!stl && !three_mf) || (per_volume && !three_mf)) return err << kUsage, 2;

  auto loaded = model::load_project(to_path(*project));
  if (!loaded) {
    err << loaded.error().to_string() << "\n";
    return 1;
  }
  const auto settings = core::ProductSettings::defaults();
  const model::Document& doc = loaded->document;
  const model::DocumentBuild built = model::build(doc, settings, &loaded->blobs);
  for (const auto& d : built.diagnostics) {
    if (d.severity == core::Severity::Error) out << d.to_string() << "\n";
  }
  const mesh::CombinedMesh c = mesh::combine(doc, built, &loaded->blobs, settings, options);

  out << fmt::format("project     {}\n", doc.name);
  for (const mesh::VolumeTessellation& v : c.volumes) {
    out << fmt::format("  {:<12}{} triangles, {} seam{}\n", v.name, v.mesh.triangle_count(),
                       v.seams.size(), v.seams.size() == 1 ? "" : "s");
  }
  const mesh::TopologyReport& t = c.weld.topology;
  if (!c.volumes.empty()) {
    out << fmt::format(
        "combined    {} triangles, {} vertices, {} shell{}, watertight {}, manifold {}, "
        "volume {:.6g} mm³\n",
        c.weld.mesh.triangle_count(), c.weld.mesh.vertex_count(), t.shells,
        t.shells == 1 ? "" : "s", t.watertight() ? "yes" : "no", t.manifold() ? "yes" : "no",
        mesh::enclosed_volume(c.weld.mesh));
  }
  for (const auto& d : c.diagnostics) out << d.to_string() << "\n";
  if (!built.ok() || !c.ok()) {
    out << "result      failed (nothing written)\n";
    return 1;
  }
  if (stl) {
    const auto bytes = mesh::write_stl(c.weld.mesh);
    if (!bytes) return err << bytes.error().to_string() << "\n", 1;
    if (!write_bytes(to_path(*stl), *bytes, err)) return 1;
    out << fmt::format("wrote       {} (STL, {} bytes)\n", *stl, bytes->size());
  }
  if (three_mf) {
    std::vector<mesh::ThreeMfObject> objects;
    if (per_volume) {
      for (const mesh::VolumeTessellation& v : c.volumes) objects.push_back({v.name, v.volume, &v.mesh});
    } else {
      objects.push_back({doc.name, core::Id::null(), &c.weld.mesh});
    }
    const auto bytes = mesh::write_3mf(objects, doc.name);
    if (!bytes) return err << bytes.error().to_string() << "\n", 1;
    if (!write_bytes(to_path(*three_mf), *bytes, err)) return 1;
    out << fmt::format("wrote       {} (3MF, {} object{}, {} bytes)\n", *three_mf, objects.size(),
                       objects.size() == 1 ? "" : "s", bytes->size());
  }
  out << "result      ok\n";
  return 0;
}

int run_import(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  constexpr std::string_view kUsage =
      "usage: vstudio-cli import <project.vsproj> <file.step|.iges|.stl|.3mf|.obj> "
      "[--name <name> | --into <volume>] [--out <project.vsproj>]\n";
  std::vector<std::string_view> positional;
  std::optional<std::string_view> name, out_path, into;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if ((args[i] == "--name" || args[i] == "--out" || args[i] == "--into") &&
        i + 1 < args.size()) {
      (args[i] == "--name" ? name : args[i] == "--out" ? out_path : into) = args[i + 1];
      ++i;
    } else if (args[i].starts_with("-")) {
      return err << kUsage, 2;
    } else {
      positional.push_back(args[i]);
    }
  }
  if (positional.size() != 2 || (name && into)) return err << kUsage, 2;
  const std::filesystem::path project_path = to_path(positional[0]);
  const std::filesystem::path file_path = to_path(positional[1]);
  auto loaded = model::load_project(project_path);
  if (!loaded) {
    err << loaded.error().to_string() << "\n";
    return 1;
  }
  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    err << "vstudio-cli: can't read " << file_path.string() << "\n";
    return 1;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  mesh::Bytes bytes(text.size());
  std::transform(text.begin(), text.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
  std::string format = file_path.extension().string();
  if (!format.empty() && format.front() == '.') format.erase(0, 1);
  const auto settings = core::ProductSettings::defaults();
  std::string volume_name;
  core::Id id;
  if (into) {
    const model::Volume* v = loaded->document.find_volume(*into);
    if (v == nullptr) {
      err << "vstudio-cli: no volume named '" << *into << "'\n";
      return 1;
    }
    volume_name = v->name;
    id = v->id;
    const core::Diagnostics problems =
        eval::import_contents(*loaded, id, bytes, format, settings);
    for (const auto& d : problems) out << d.to_string() << "\n";
    if (problems.has_errors()) return 1;
  } else {
    volume_name = name ? std::string(*name) : file_path.stem().string();
    const auto added = eval::import_volume(*loaded, bytes, format, volume_name, settings);
    if (!added) {
      out << added.error().to_string() << "\n";
      return 1;
    }
    id = *added;
  }
  const std::filesystem::path target = out_path ? to_path(*out_path) : project_path;
  const auto layout = std::filesystem::is_directory(project_path) ? model::ProjectLayout::Folder
                                                                  : model::ProjectLayout::Zip;
  const core::Diagnostics saved = model::save_project(*loaded, target, layout);
  for (const auto& d : saved) out << d.to_string() << "\n";
  if (saved.has_errors()) return 1;
  out << fmt::format("imported    {} as {}static volume '{}' ({})\nsaved       {}\n",
                     file_path.filename().string(), into ? "the contents of " : "", volume_name,
                     id.to_string(), target.string());
  return 0;
}

}  // namespace vs::cli

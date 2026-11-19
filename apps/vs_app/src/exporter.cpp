#include "exporter.hpp"

#include <fstream>
#include <variant>

namespace vs::app {

std::string_view extension(ExportFormat format) {
  return format == ExportFormat::Stl ? "stl" : "3mf";
}

ExportResult export_mesh(const eval::Evaluation& evaluation, const model::BlobStore& blobs,
                         const core::ProductSettings& settings, ExportFormat format) {
  ExportResult out;
  if (!evaluation.checked) {
    out.diagnostics.add(core::Diagnostic::error(
        "app.export.unchecked", "The project has errors that stop it from building; fix them first."));
    return out;
  }
  mesh::GeneratedMeshes generated;
  for (std::size_t i = 0; i < evaluation.volumes.size(); ++i) {
    const eval::VolumeResult& r = evaluation.volumes[i];
    if (r.contents && r.contents->mesh &&
        !std::holds_alternative<model::StaticContents>(evaluation.document.volumes[i].contents)) {
      generated.emplace(r.volume, &*r.contents->mesh);
    }
  }
  const mesh::CombinedMesh combined =
      mesh::combine(evaluation.document, evaluation.build, &blobs, settings, {}, generated);
  out.diagnostics.append(combined.diagnostics);
  if (!combined.ok()) return out;

  core::Result<mesh::Bytes> bytes = core::Diagnostic::error("app.export.format", "unknown format");
  if (format == ExportFormat::Stl) {
    bytes = mesh::write_stl(combined.weld.mesh);
    out.triangles = combined.weld.mesh.triangle_count();
    out.objects = 1;
  } else {
    std::vector<mesh::ThreeMfObject> objects;
    for (const mesh::VolumeTessellation& v : combined.volumes) {
      objects.push_back({v.name, v.volume, &v.mesh});
      out.triangles += v.mesh.triangle_count();
    }
    out.objects = objects.size();
    bytes = mesh::write_3mf(objects, evaluation.document.name);
  }
  if (!bytes) {
    out.diagnostics.add(bytes.error());
    return out;
  }
  out.bytes = std::move(bytes).value();
  return out;
}

core::Diagnostics write_file(const std::filesystem::path& path, const mesh::Bytes& bytes) {
  core::Diagnostics d;
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (f) {
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  if (!f) {
    const std::u8string name = path.u8string();
    std::string label(name.begin(), name.end());
    d.add(core::Diagnostic::error("app.export.io", "Can't write " + label,
                                  {{core::SubjectKind::File, {}, label}}));
  }
  return d;
}

}  // namespace vs::app

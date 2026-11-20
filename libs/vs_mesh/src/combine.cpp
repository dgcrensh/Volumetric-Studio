#include "vs/mesh/combine.hpp"

#include <algorithm>

#include "vs/mesh/io.hpp"

namespace vs::mesh {

using core::Diagnostic;

CombinedMesh combine(const model::Document& document, const model::DocumentBuild& build,
                     const model::BlobStore* blobs, const core::ProductSettings& settings,
                     const CombineOptions& options, const GeneratedMeshes& generated) {
  CombinedMesh out;
  out.seams = build_seams(document, build, settings);
  out.diagnostics.append(out.seams.diagnostics);

  for (const model::Volume& v : document.volumes) {
    if (v.keep_out || v.composite()) continue;  // a composite's children mesh it (L3)
    const core::SubjectRef subject = document.volume_subject(v.id);
    const model::VolumeBuild* vb = build.find_volume(v.id);
    const auto skip = [&](const std::string& why) {
      out.diagnostics.add(Diagnostic::info("mesh.combine.skipped",
                                           "Not in the combined mesh: " + why, {subject}));
    };
    std::vector<const Seam*> seams;
    for (const Seam& s : out.seams.seams) {
      if (std::find(s.volumes.begin(), s.volumes.end(), v.id) != s.volumes.end()) {
        seams.push_back(&s);
      }
    }

    VolumeTessellation vt;
    vt.volume = v.id;
    vt.name = subject.label;
    const auto* stat = std::get_if<model::StaticContents>(&v.contents);
    const auto made = generated.find(v.id);
    if (made != generated.end() && made->second != nullptr) {
      vt.mesh = *made->second;
      for (const Seam* s : seams) {
        if (std::any_of(vt.mesh.seam_vertices.begin(), vt.mesh.seam_vertices.end(),
                        [&](const core::Id& id) { return s->find(id).has_value(); })) {
          vt.seams.push_back(s->surface);
        }
      }
    } else if (vb != nullptr && !vb->contents.is_empty()) {
      VolumeMesh m = tessellate(vb->contents, subject, seams, settings, options.tessellation);
      out.diagnostics.append(m.diagnostics);
      vt.mesh = std::move(m.mesh);
      vt.seams = std::move(m.seams);
    } else if (stat != nullptr && !stat->blob.empty() && is_mesh_format(stat->format)) {
      const model::Blob* blob = nullptr;
      if (blobs != nullptr) {
        const auto it = blobs->find(stat->blob);
        if (it != blobs->end()) blob = &it->second;
      }
      if (blob == nullptr) {
        out.diagnostics.add(Diagnostic::error(
            "mesh.combine.blob", "The volume's mesh file '" + stat->blob + "' isn't available",
            {subject}));
        continue;
      }
      core::Result<Mesh> m = read_mesh(*blob, stat->format);
      if (!m) {
        Diagnostic d = m.error();
        d.subjects.insert(d.subjects.begin(), subject);
        out.diagnostics.add(std::move(d));
        continue;
      }
      assign_volume(*m, v.id);
      vt.mesh = std::move(m).value();
      // A baked procedural volume's file was made against the seams: give
      // its seam vertices their ids back so it welds (M12).
      if (attach_seam_vertices(vt.mesh, seams) > 0) {
        for (const Seam* s : seams) {
          if (std::any_of(vt.mesh.seam_vertices.begin(), vt.mesh.seam_vertices.end(),
                          [&](const core::Id& id) { return s->find(id).has_value(); })) {
            vt.seams.push_back(s->surface);
          }
        }
      }
    } else if (v.contents_empty() && options.placeholders && vb != nullptr &&
               !vb->boundary.is_empty()) {
      VolumeMesh m = tessellate(vb->boundary, subject, seams, settings, options.tessellation);
      out.diagnostics.append(m.diagnostics);
      vt.mesh = std::move(m.mesh);
      vt.seams = std::move(m.seams);
    } else if (v.contents_empty()) {
      skip("a placeholder, with no contents yet");
      continue;
    } else if (v.kind() == model::VolumeKind::Procedural) {
      skip("its procedural contents have no mesh");
      continue;
    } else {
      skip("its contents weren't built");
      continue;
    }
    if (!vt.mesh.empty()) out.volumes.push_back(std::move(vt));
  }

  if (out.volumes.empty()) {
    out.diagnostics.add(
        Diagnostic::error("mesh.combine.empty", "No volume has geometry to put in the mesh"));
    return out;
  }
  std::vector<Mesh> meshes;
  meshes.reserve(out.volumes.size());
  for (const VolumeTessellation& vt : out.volumes) meshes.push_back(vt.mesh);
  out.weld = weld(meshes, out.seams);
  out.diagnostics.append(out.weld.diagnostics);
  return out;
}

}  // namespace vs::mesh

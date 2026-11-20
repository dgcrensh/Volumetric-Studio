#include "vs/eval/step_export.hpp"

#include <fmt/format.h>

namespace vs::eval {

using core::Diagnostic;

namespace {

bool current(const VolumeResult& r) {
  return r.status == model::VolumeStatus::Current && !r.stale_contents;
}

}  // namespace

StepExport step_parts(const Evaluation& evaluation, const StepExportOptions& options) {
  StepExport out;
  const model::Document& doc = evaluation.document;
  std::vector<kernel::NamedShape> parts;

  for (std::size_t i = 0; i < doc.volumes.size() && i < evaluation.volumes.size(); ++i) {
    const model::Volume& v = doc.volumes[i];
    if (v.keep_out) continue;
    const VolumeResult& r = evaluation.volumes[i];
    const model::VolumeBuild* vb = evaluation.build.find_volume(v.id);
    const kernel::Solid boundary = vb != nullptr ? vb->boundary : kernel::Solid{};

    if (!current(r)) {
      out.diagnostics.add(Diagnostic::error(
          "eval.export.not_current",
          fmt::format("Volume '{}' is {}{}: only current results can be exported", v.name,
                      model::to_string(r.status), r.stale_contents ? " (showing stale contents)" : ""),
          {doc.volume_subject(v.id)}));
      continue;
    }

    const auto add_boundary = [&] {
      if (boundary.is_empty()) {
        out.diagnostics.add(Diagnostic::error(
            "eval.export.not_current",
            fmt::format("Volume '{}' has no boundary to export", v.name), {doc.boundary_subject(v.id)}));
        return;
      }
      parts.push_back({v.name, boundary});
      ++out.boundaries;
    };

    const ContentsPtr& c = r.contents;
    if (v.contents_empty() || !c || (!c->mesh && c->solid.is_empty())) {
      out.diagnostics.add(Diagnostic::info(
          "eval.export.placeholder",
          fmt::format("Volume '{}' has no contents yet, so its boundary is exported", v.name),
          {doc.volume_subject(v.id)}));
      add_boundary();
      continue;
    }
    if (c->mesh) {
      if (v.kind() == model::VolumeKind::Procedural && options.procedural == ProceduralAs::Boundary) {
        add_boundary();
        continue;
      }
      auto shape = kernel::mesh_shape(c->mesh->positions, c->mesh->indices);
      if (!shape) {
        Diagnostic d = shape.error();
        d.subjects.insert(d.subjects.begin(), doc.volume_subject(v.id));
        out.diagnostics.add(std::move(d));
        continue;
      }
      parts.push_back({v.name, *shape});
      ++out.meshes;
      continue;
    }
    parts.push_back({v.name, c->solid});
    ++out.breps;
  }

  if (options.boundaries) {
    for (const auto& v : doc.volumes) {
      const model::VolumeBuild* vb = evaluation.build.find_volume(v.id);
      if (vb != nullptr && !vb->boundary.is_empty()) {
        parts.push_back({fmt::format("{} boundary", v.name), vb->boundary});
      }
    }
  }
  if (options.connection_surfaces) {
    for (const auto& s : doc.connections) {
      const model::ConnectionBuild* cb = evaluation.build.find_connection(s.id);
      if (cb != nullptr && cb->ok && !cb->patch.is_empty()) parts.push_back({s.name, cb->patch});
    }
  }

  if (!out.diagnostics.has_errors()) out.parts = std::move(parts);
  if (out.parts.empty() && !out.diagnostics.has_errors()) {
    out.diagnostics.add(Diagnostic::error("eval.export.empty", "The design has no volumes to export"));
  }
  return out;
}

StepExport export_step(const Evaluation& evaluation, const std::filesystem::path& path,
                       const StepExportOptions& options) {
  StepExport out = step_parts(evaluation, options);
  if (!out.ok()) return out;
  const std::string name = evaluation.document.name.empty() ? "Design" : evaluation.document.name;
  Diagnostics written = kernel::write_step(out.parts, path, name);
  if (written.has_errors()) out.parts.clear();
  out.diagnostics.append(std::move(written));
  return out;
}

}  // namespace vs::eval

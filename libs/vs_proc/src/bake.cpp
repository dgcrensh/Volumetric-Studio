#include "vs/proc/bake.hpp"

#include <span>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "vs/mesh/io.hpp"
#include "vs/model/check.hpp"

namespace vs::proc {

using core::Diagnostic;
using core::Diagnostics;

namespace {

Diagnostics missing(const core::Id& volume) {
  Diagnostics d;
  d.add(Diagnostic::error("model.reference.missing",
                          fmt::format("There's no volume {} in the document", volume.to_string())));
  return d;
}

bool blob_used(const model::Document& doc, const std::string& blob) {
  for (const auto& v : doc.volumes) {
    const auto* s = std::get_if<model::StaticContents>(&v.contents);
    if (s != nullptr && s->blob == blob) return true;
  }
  return false;
}

}  // namespace

bool is_baked(const model::Volume& volume) {
  const auto* s = std::get_if<model::StaticContents>(&volume.contents);
  return s != nullptr && !s->baked_graph.empty();
}

Diagnostics bake(model::Project& project, const core::Id& volume, const mesh::Mesh& mesh) {
  model::Volume* v = project.document.find_volume(volume);
  if (v == nullptr) return missing(volume);
  const core::SubjectRef self = project.document.volume_subject(volume);
  Diagnostics out;
  const auto* contents = std::get_if<model::ProceduralContents>(&v->contents);
  if (contents == nullptr || v->keep_out || contents->graph.empty()) {
    out.add(Diagnostic::error("proc.bake.kind",
                              fmt::format("'{}' isn't a procedural volume with a graph, so there's "
                                          "nothing to bake",
                                          self.label),
                              {self}));
    return out;
  }
  if (mesh.empty()) {
    out.add(Diagnostic::error(
        "proc.bake.empty",
        fmt::format("'{}' has no generated mesh to bake: generate it first", self.label), {self}));
    return out;
  }
  const mesh::ThreeMfObject object{v->name, volume, &mesh};
  auto bytes = mesh::write_3mf(std::span(&object, 1), v->name);
  if (!bytes) {
    out.add(Diagnostic::error(
        "proc.bake.write",
        fmt::format("'{}' couldn't be baked: {}", self.label, bytes.error().message), {self}));
    return out;
  }
  const std::string graph = contents->graph;
  model::Project next = project;
  const std::string blob = next.add_blob(std::move(bytes).value(), "3mf");
  out = model::edit(next.document, [&](model::Document& doc) {
    doc.find_volume(volume)->contents = model::StaticContents{blob, "3mf", graph};
  });
  if (out.has_errors()) return out;
  project = std::move(next);
  return out;
}

Diagnostics unbake(model::Project& project, const core::Id& volume) {
  model::Volume* v = project.document.find_volume(volume);
  if (v == nullptr) return missing(volume);
  const core::SubjectRef self = project.document.volume_subject(volume);
  Diagnostics out;
  if (!is_baked(*v)) {
    out.add(Diagnostic::error(
        "proc.bake.kind",
        fmt::format("'{}' wasn't baked from a graph, so it can't be unbaked", self.label), {self}));
    return out;
  }
  const auto stat = std::get<model::StaticContents>(v->contents);
  model::Project next = project;
  out = model::edit(next.document, [&](model::Document& doc) {
    doc.find_volume(volume)->contents = model::ProceduralContents{stat.baked_graph};
  });
  if (out.has_errors()) return out;
  if (!blob_used(next.document, stat.blob)) next.blobs.erase(stat.blob);
  project = std::move(next);
  return out;
}

}  // namespace vs::proc

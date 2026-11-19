#include "vs/mesh/import.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include "vs/feature/tree.hpp"
#include "vs/mesh/io.hpp"
#include "vs/model/check.hpp"

namespace vs::mesh {

using core::Diagnostic;

core::Result<core::Id> import_mesh_volume(model::Project& project, std::span<const std::byte> file,
                                          std::string_view format, std::string_view name,
                                          const core::ProductSettings& settings) {
  core::Result<Mesh> mesh = read_mesh(file, format);
  if (!mesh) return mesh.error();
  const core::fbox3 box = mesh->bounds();
  const core::dvec3 lo = core::to_fp64(box.min), hi = core::to_fp64(box.max);
  const core::dvec3 size = hi - lo;
  if (std::min({size.x, size.y, size.z}) < settings.min_feature_size_mm()) {
    return Diagnostic::error("mesh.import.flat",
                             "The mesh is too thin in some direction to have a box boundary");
  }
  std::string ext(format);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  model::Project next = project;
  const std::string blob =
      next.add_blob(model::Blob(file.begin(), file.end()), ext);
  core::Id volume;
  const core::Diagnostics problems = model::edit(next.document, [&](model::Document& doc) {
    const auto point = [&](const std::string& label, const core::dvec3& p) {
      return doc.layout.add(feature::DatumPointFeature{feature::PointAt{
                                feature::Param::of(p.x), feature::Param::of(p.y),
                                feature::Param::of(p.z)}},
                            label);
    };
    const std::string n(name);
    const core::Id a = point(n + " min", lo);
    const core::Id b = point(n + " max", hi);
    model::Volume v;
    v.id = volume = core::Id::random();
    v.name = n;
    v.boundary = model::BoxBoundary{feature::FeatureRef{a}, feature::FeatureRef{b}};
    v.contents = model::StaticContents{blob, ext, {}};
    doc.volumes.push_back(std::move(v));
  });
  if (problems.has_errors()) {
    for (const Diagnostic& d : problems) {
      if (d.severity == core::Severity::Error) return d;
    }
  }
  project = std::move(next);
  return volume;
}

}  // namespace vs::mesh

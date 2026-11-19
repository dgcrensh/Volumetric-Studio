#include "vs/eval/import.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

#include <fmt/format.h>

#include "vs/feature/tree.hpp"
#include "vs/kernel/exchange.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/mesh/import.hpp"
#include "vs/mesh/io.hpp"
#include "vs/model/check.hpp"

namespace vs::eval {

using core::Diagnostic;

std::string_view import_format(std::string_view extension) {
  std::string e(extension);
  if (!e.empty() && e.front() == '.') e.erase(0, 1);
  std::transform(e.begin(), e.end(), e.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (e == "step" || e == "stp") return "step";
  if (e == "iges" || e == "igs") return "iges";
  if (e == "stl") return "stl";
  if (e == "3mf") return "3mf";
  if (e == "obj") return "obj";
  return {};
}

namespace {

bool is_cad(std::string_view format) { return format == "step" || format == "iges"; }

// The file's one solid. The exchange layer reads files, so it gets a
// temporary one.
core::Result<kernel::Solid> read_solid(std::span<const std::byte> file, std::string_view format) {
  const auto tmp = std::filesystem::temp_directory_path() /
                   fmt::format("vstudio-import-{}.{}", core::Id::random().to_string(), format);
  {
    std::ofstream f(tmp, std::ios::binary);
    f.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  }
  auto shape = format == "step" ? kernel::read_step(tmp) : kernel::read_iges(tmp);
  std::error_code ignored;
  std::filesystem::remove(tmp, ignored);
  if (!shape) {
    return Diagnostic::error("eval.import.read",
                             fmt::format("The {} file doesn't read: {}", format,
                                         shape.error().message));
  }
  auto solid = kernel::Solid::from(*shape);
  if (!solid) {
    const auto parts = kernel::solids(*shape);
    if (parts.size() == 1) return parts.front();
    return Diagnostic::error("eval.import.read",
                             fmt::format("The {} file holds {} solids; a static volume takes one",
                                         format, parts.size()));
  }
  return *solid;
}

Diagnostic first_error(const core::Diagnostics& problems) {
  for (const Diagnostic& d : problems) {
    if (d.severity == core::Severity::Error) return d;
  }
  return problems.empty() ? Diagnostic::error("eval.import.read", "The import failed")
                          : *problems.begin();
}

core::Diagnostics single(Diagnostic d) {
  core::Diagnostics out;
  out.add(std::move(d));
  return out;
}

Diagnostic bad_format(std::string_view format) {
  return Diagnostic::error("eval.import.format",
                           fmt::format("'{}' isn't a format that imports (STEP, IGES, STL, 3MF "
                                       "or OBJ)",
                                       format));
}

}  // namespace

core::Result<core::Id> import_volume(model::Project& project, std::span<const std::byte> file,
                                     std::string_view format_in, std::string_view name,
                                     const core::ProductSettings& settings) {
  const std::string_view format = import_format(format_in);
  if (format.empty()) return bad_format(format_in);
  if (!is_cad(format)) return mesh::import_mesh_volume(project, file, format, name, settings);

  auto solid = read_solid(file, format);
  if (!solid) return solid.error();
  const core::dbox3 box = kernel::bounding_box(*solid);
  const core::dvec3 size = box.max - box.min;
  if (std::min({size.x, size.y, size.z}) < settings.min_feature_size_mm()) {
    return Diagnostic::error("eval.import.flat",
                             "The solid is too thin in some direction to have a box boundary");
  }

  model::Project next = project;
  const std::string blob = next.add_blob(model::Blob(file.begin(), file.end()), format);
  core::Id volume;
  const core::Diagnostics problems = model::edit(next.document, [&](model::Document& doc) {
    const auto point = [&](const std::string& label, const core::dvec3& p) {
      return doc.layout.add(feature::DatumPointFeature{feature::PointAt{
                                feature::Param::of(p.x), feature::Param::of(p.y),
                                feature::Param::of(p.z)}},
                            label);
    };
    const std::string n(name);
    const core::Id a = point(n + " min", box.min);
    const core::Id b = point(n + " max", box.max);
    model::Volume v;
    v.id = volume = core::Id::random();
    v.name = n;
    v.boundary = model::BoxBoundary{feature::FeatureRef{a}, feature::FeatureRef{b}};
    v.contents = model::StaticContents{blob, std::string(format), {}};
    doc.volumes.push_back(std::move(v));
  });
  if (problems.has_errors()) return first_error(problems);
  project = std::move(next);
  return volume;
}

core::Diagnostics import_contents(model::Project& project, const core::Id& volume,
                                  std::span<const std::byte> file, std::string_view format_in,
                                  const core::ProductSettings& settings) {
  (void)settings;
  const std::string_view format = import_format(format_in);
  if (format.empty()) return single(bad_format(format_in));
  const model::Volume* target = project.document.find_volume(volume);
  if (target == nullptr || target->keep_out) {
    return single(Diagnostic::error(
        "eval.import.volume",
        target == nullptr ? "There is no such volume to import into"
                          : fmt::format("'{}' is a keep-out and can't have contents",
                                        target->name)));
  }
  if (is_cad(format)) {
    auto solid = read_solid(file, format);
    if (!solid) return single(solid.error());
  } else {
    auto m = mesh::read_mesh(file, format);
    if (!m) return single(m.error());
  }

  model::Project next = project;
  const std::string blob = next.add_blob(model::Blob(file.begin(), file.end()), format);
  core::Diagnostics problems = model::edit(next.document, [&](model::Document& doc) {
    doc.find_volume(volume)->contents = model::StaticContents{blob, std::string(format), {}};
  });
  if (problems.has_errors()) return problems;
  project = std::move(next);
  return problems;
}

}  // namespace vs::eval

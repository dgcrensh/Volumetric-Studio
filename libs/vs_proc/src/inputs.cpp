#include "vs/proc/inputs.hpp"
#include "vs/model/hierarchy.hpp"

#include <algorithm>

#include <fmt/format.h>

#include "vs/feature/rebuild.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/kernel/tessellate.hpp"
#include "vs/mesh/tessellate.hpp"

namespace vs::proc {

using core::Diagnostic;

const ConnectionInput* GraphInputs::find_connection(const Id& surface) const {
  for (const auto& c : connections) {
    if (c.id == surface) return &c;
  }
  return nullptr;
}

namespace {

// The first error's code and message, for wrapping in a proc.input.* one.
std::string first_error(const core::Diagnostics& diagnostics) {
  for (const auto& d : diagnostics) {
    if (d.is_error()) return fmt::format("[{}] {}", d.code, d.message);
  }
  return "no details";
}

}  // namespace

core::Result<GraphInputs> gather_inputs(const model::Document& document,
                                        const model::DocumentBuild& layout, std::size_t index,
                                        const core::ProductSettings& settings,
                                        const GatherOptions& gather) {
  const model::Volume& v = document.volumes.at(index);
  const model::VolumeBuild& vb = layout.volumes.at(index);
  GraphInputs in;
  in.volume = v.id;
  in.volume_subject = document.volume_subject(v.id);
  in.seed = v.seed;
  in.settings = settings;
  for (const auto& [name, value] : vb.parameters) in.parameters.emplace(name, value);

  if (vb.boundary.is_empty()) {
    return Diagnostic::error(
        "model.contents.no_boundary",
        fmt::format("'{}': its boundary failed, so there's nothing to fill", in.volume_subject.label),
        {in.volume_subject});
  }
  mesh::TessellationOptions options;
  options.params.deflection = kInputDeflection;
  const mesh::VolumeMesh vm = mesh::tessellate(vb.boundary, in.volume_subject, {}, settings, options);
  if (!vm.ok()) {
    return Diagnostic::error(
        "proc.input.boundary",
        fmt::format("'{}': its boundary can't be turned into a distance field: {}",
                    in.volume_subject.label, first_error(vm.diagnostics)),
        {in.volume_subject});
  }
  in.boundary = std::make_shared<const TriangleDistance>(vm.mesh, true);
  in.bounds = kernel::bounding_box(vb.boundary);

  // V1: the named faces asked for, from one tessellation of the boundary.
  if (!gather.boundary_faces.empty()) {
    kernel::TessellationParams params;
    params.deflection = kInputDeflection;
    const auto tess = kernel::tessellate(vb.boundary, params, settings);
    if (!tess) {
      return Diagnostic::error(
          "proc.input.boundary",
          fmt::format("'{}': its boundary's faces can't be tessellated: [{}] {}",
                      in.volume_subject.label, tess.error().code, tess.error().message),
          {in.volume_subject});
    }
    for (const auto& name : gather.boundary_faces) {
      if (in.boundary_faces.contains(name)) continue;
      const auto named = std::find_if(vb.boundary_faces.begin(), vb.boundary_faces.end(),
                                      [&](const feature::NamedFace& f) { return f.name == name; });
      if (named == vb.boundary_faces.end()) {
        return Diagnostic::error(
            "proc.reference.boundary_face",
            fmt::format("'{}': its graph reads boundary face '{}', which its boundary doesn't have",
                        in.volume_subject.label, name),
            {in.volume_subject});
      }
      mesh::Mesh face;
      for (const auto& f : tess->faces) {
        if (!feature::on_named_face(*named, f.face, settings.linear_tolerance_mm)) continue;
        const auto base = static_cast<std::uint32_t>(face.vertex_count());
        for (const auto& p : f.points) face.add_vertex(core::to_fp32(p));
        for (const auto& t : f.triangles) face.add_triangle(base + t[0], base + t[1], base + t[2], v.id);
      }
      if (face.triangle_count() == 0) {
        return Diagnostic::error(
            "proc.reference.boundary_face",
            fmt::format("'{}': its graph reads boundary face '{}', but no face of the built "
                        "boundary lies on it (a keep-out may have cut it away)",
                        in.volume_subject.label, name),
            {in.volume_subject});
      }
      in.boundary_faces.emplace(name, std::make_shared<const TriangleDistance>(face, false));
    }
  }

  for (const auto& s : document.connections) {
    if (!model::joins(document, s, v.id)) continue;
    const core::SubjectRef surface = document.connection_subject(s.id);
    const model::ConnectionBuild* cb = layout.find_connection(s.id);
    if (cb == nullptr || !cb->ok || cb->patch.is_empty()) {
      return Diagnostic::error(
          "proc.input.surface",
          fmt::format("'{}': its connection surface '{}' failed", in.volume_subject.label,
                      surface.label),
          {in.volume_subject, surface});
    }
    kernel::TessellationParams params;
    params.deflection = kInputDeflection;
    const auto tess = kernel::tessellate(cb->patch, params, settings);
    if (!tess) {
      return Diagnostic::error(
          "proc.input.surface",
          fmt::format("'{}': its connection surface '{}' can't be tessellated: [{}] {}",
                      in.volume_subject.label, surface.label, tess.error().code,
                      tess.error().message),
          {in.volume_subject, surface});
    }
    mesh::Mesh patch;
    for (const auto& f : tess->faces) {
      const auto base = static_cast<std::uint32_t>(patch.vertex_count());
      for (const auto& p : f.points) patch.add_vertex(core::to_fp32(p));
      for (const auto& t : f.triangles) patch.add_triangle(base + t[0], base + t[1], base + t[2], s.id);
    }

    ConnectionInput c;
    c.id = s.id;
    c.name = s.name;
    c.type = s.type();
    c.origin = cb->frame.origin;
    c.x_axis = cb->frame.x_axis;
    c.normal = cb->frame.normal();
    if (cb->cylinder) c.radius = cb->cylinder->radius;
    c.area = cb->area;
    c.gap = cb->gap;
    c.min_wall = cb->min_wall;
    if (const auto* flange = std::get_if<model::BoltedFlangeParams>(&s.params)) {
      c.thread = flange->thread;
    }
    c.hole_centres = cb->hole_centres;
    c.patch = std::make_shared<const TriangleDistance>(patch, false);

    core::Hasher h;
    h.add(std::string_view("vstudio.proc.connection_input"));
    h.add(c.id).add(c.type).add(c.origin).add(c.x_axis).add(c.normal).add(c.area);
    h.add(c.gap.has_value()).add(c.gap.value_or(0.0));
    h.add(c.min_wall.has_value()).add(c.min_wall.value_or(0.0));
    h.add(c.thread);
    h.add(static_cast<std::uint64_t>(c.hole_centres.size()));
    for (const auto& p : c.hole_centres) h.add(p);
    h.add(c.patch->hash());
    if (c.radius) h.add(std::string_view("curved")).add(*c.radius);  // planar hashes unchanged
    c.hash = h.finalize();
    in.connections.push_back(std::move(c));
  }
  return in;
}

}  // namespace vs::proc

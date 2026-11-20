#include "vs/mesh/seam_library.hpp"
#include "vs/model/hierarchy.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "vs/kernel/queries.hpp"

namespace vs::mesh {
namespace {

using core::Diagnostic;
using core::Result;

dvec2 to_frame(const sketch::SketchPlane& f, const dvec3& p) {
  const dvec3 d = p - f.origin;
  return {core::dot(d, f.x_axis) / core::dot(f.x_axis, f.x_axis),
          core::dot(d, f.y_axis) / core::dot(f.y_axis, f.y_axis)};
}

}  // namespace

const Seam* SeamLibrary::find(const core::Id& surface) const {
  for (const Seam& s : seams) {
    if (s.surface == surface) return &s;
  }
  return nullptr;
}

std::optional<SeamVertexRef> SeamLibrary::find_vertex(const core::Id& vertex) const {
  const auto it = vertices_.find(vertex);
  if (it == vertices_.end()) return std::nullopt;
  return it->second;
}

core::SubjectRef SeamLibrary::volume_subject(const core::Id& volume) const {
  const auto it = volume_names.find(volume);
  return {core::SubjectKind::Volume, volume,
          it != volume_names.end() ? it->second : volume.to_string()};
}

void SeamLibrary::add(Seam seam) {
  const auto s = static_cast<std::uint32_t>(seams.size());
  for (std::uint32_t i = 0; i < seam.vertices.size(); ++i) {
    vertices_.emplace(seam.vertices[i].id, SeamVertexRef{s, i});
  }
  seams.push_back(std::move(seam));
}

Result<SeamOutline> seam_outline(const model::ConnectionSurface& surface,
                                 const model::ConnectionBuild& build) {
  const core::SubjectRef subject{core::SubjectKind::ConnectionSurface, surface.id, surface.name};
  SeamOutline outline;
  outline.frame = build.frame;
  if (const auto& cyl = build.cylinder) {
    // A cylinder (L2): its unrolled rectangle, or a band's two ends.
    outline.radius = cyl->radius;
    outline.band = cyl->band();
    const double x0 = cyl->radius * cyl->start_angle;
    const double x1 = cyl->radius * (cyl->start_angle + cyl->sweep);
    const double y0 = cyl->start;
    const double y1 = cyl->start + cyl->length;
    const dvec2 c00{x0, y0}, c10{x1, y0}, c11{x1, y1}, c01{x0, y1};
    if (outline.band) {
      outline.loops.push_back(OutlineLoop{{OutlineSegment::line(c00, c10)}});
      outline.loops.push_back(OutlineLoop{{OutlineSegment::line(c11, c01)}});
    } else {
      outline.loops.push_back(OutlineLoop{{OutlineSegment::line(c00, c10),
                                           OutlineSegment::line(c10, c11),
                                           OutlineSegment::line(c11, c01),
                                           OutlineSegment::line(c01, c00)}});
    }
    if (std::holds_alternative<model::BoltedFlangeParams>(surface.params)) {
      return Diagnostic::error("mesh.seam.curve", "A bolted flange can't be curved", {subject});
    }
    return outline;
  }
  const dvec3 n = build.frame.normal();
  for (const kernel::FaceLoop& fl : kernel::face_loops(build.patch)) {
    OutlineLoop loop;
    for (const kernel::LoopEdge& e : fl.edges) {
      switch (e.kind) {
        case kernel::LoopEdge::Kind::Line:
          loop.segments.push_back(
              OutlineSegment::line(to_frame(outline.frame, e.start), to_frame(outline.frame, e.end)));
          break;
        case kernel::LoopEdge::Kind::Circle: {
          const double along = core::dot(e.axis, n);
          if (std::abs(std::abs(along) - 1.0) > 1e-9) {
            return Diagnostic::error("mesh.seam.curve",
                                     "An arc of the outline isn't in the surface's plane", {subject});
          }
          const dvec2 c = to_frame(outline.frame, e.centre);
          const dvec2 s = to_frame(outline.frame, e.start) - c;
          const double a0 = std::atan2(s.y, s.x);
          loop.segments.push_back(
              OutlineSegment::arc(c, e.radius, a0, along > 0 ? e.sweep : -e.sweep));
          break;
        }
        case kernel::LoopEdge::Kind::Other:
          return Diagnostic::error("mesh.seam.curve",
                                   "An edge of the outline is neither a line nor an arc", {subject});
      }
    }
    if (!loop.segments.empty()) outline.loops.push_back(std::move(loop));
  }
  if (outline.loops.empty()) {
    return Diagnostic::error("mesh.seam.outline", "The surface's patch has no outline", {subject});
  }
  if (const auto* flange = std::get_if<model::BoltedFlangeParams>(&surface.params)) {
    const auto thread = model::thread_size(flange->thread);
    if (!thread) {
      return Diagnostic::error("mesh.seam.thread",
                               "Unknown thread size '" + flange->thread + "'", {subject});
    }
    for (const dvec3& centre : build.hole_centres) {
      OutlineLoop hole;
      hole.segments.push_back(
          OutlineSegment::circle(to_frame(outline.frame, centre), thread->clearance_hole / 2.0));
      outline.loops.push_back(std::move(hole));
    }
  }
  return outline;
}

SeamLibrary build_seams(const model::Document& document, const model::DocumentBuild& build,
                        const core::ProductSettings& settings,
                        const std::optional<core::Id>& volume) {
  SeamLibrary library;
  for (const model::Volume& v : document.volumes) {
    library.volume_names.emplace(v.id, document.volume_subject(v.id).label);
  }
  for (const model::ConnectionSurface& surface : document.connections) {
    if (surface.type() == model::ConnectionType::Clearance) continue;
    if (volume && !model::joins(document, surface, *volume)) {
      continue;
    }
    const model::ConnectionBuild* cb = build.find_connection(surface.id);
    if (cb == nullptr || !cb->ok || cb->patch.is_empty()) continue;
    Result<SeamOutline> outline = seam_outline(surface, *cb);
    if (!outline) {
      library.diagnostics.add(outline.error());
      continue;
    }
    const double spacing =
        surface.seam.spacing > 0 ? surface.seam.spacing : derive_spacing(*outline, settings);
    Result<Seam> seam =
        make_seam(document.connection_subject(surface.id), *outline, spacing, settings);
    if (!seam) {
      library.diagnostics.add(seam.error());
      continue;
    }
    // The volumes whose contents meet it: delegations followed down (L3).
    seam->volumes = model::seam_sides(document, surface);
    library.add(std::move(seam).value());
  }
  return library;
}

}  // namespace vs::mesh

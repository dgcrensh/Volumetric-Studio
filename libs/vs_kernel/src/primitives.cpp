#include "vs/kernel/primitives.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>

#include "occt.hpp"
#include "vs/kernel/check.hpp"
#include "vs/kernel/queries.hpp"

namespace vs::kernel {

using detail::check_in_working_space;
using detail::check_length;
using detail::from_occ;
using detail::is_finite;
using detail::topo;
using detail::to_occ;

// --- loops ------------------------------------------------------------------

Loop Loop::polygon(std::span<const dvec3> points) {
  Loop loop;
  for (std::size_t i = 0; i < points.size(); ++i) {
    loop.segments.emplace_back(LineSegment{points[i], points[(i + 1) % points.size()]});
  }
  return loop;
}

Loop Loop::circle(const dvec3& center, const dvec3& normal, double radius) {
  // Any unit vector perpendicular to the normal gives the start point.
  const dvec3 n = core::normalize(normal);
  const dvec3 helper = std::abs(n.x) < 0.9 ? dvec3{1, 0, 0} : dvec3{0, 1, 0};
  const dvec3 u = core::normalize(core::cross(n, helper));
  const dvec3 v = core::cross(n, u);
  const dvec3 a = center + u * radius;
  const dvec3 b = center + v * radius;
  const dvec3 c = center - u * radius;
  const dvec3 d = center - v * radius;
  Loop loop;
  loop.segments.emplace_back(ArcSegment{a, b, c});
  loop.segments.emplace_back(ArcSegment{c, d, a});
  return loop;
}

namespace {

dvec3 segment_start(const ProfileSegment& s) {
  return std::visit([](const auto& seg) { return seg.start; }, s);
}
dvec3 segment_end(const ProfileSegment& s) {
  return std::visit([](const auto& seg) { return seg.end; }, s);
}

// Builds a closed wire whose consecutive edges share vertices exactly.
Result<TopoDS_Wire> make_wire(const Loop& loop, const ProductSettings& settings) {
  const double tol = settings.linear_tolerance_mm;
  const auto& segs = loop.segments;
  if (segs.empty()) {
    return Diagnostic::error("kernel.profile.empty_loop", "A profile loop has no segments");
  }
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const dvec3 end = segment_end(segs[i]);
    const dvec3 next_start = segment_start(segs[(i + 1) % segs.size()]);
    if (!is_finite(end) || !is_finite(next_start)) {
      return Diagnostic::error("kernel.profile.not_finite",
                               "A profile point is not a finite number");
    }
    if (core::distance(end, next_start) > tol) {
      return Diagnostic::error(
          "kernel.profile.open_loop",
          fmt::format("Profile segment {} ends {} mm away from the start of the next one", i,
                      core::distance(end, next_start)));
    }
  }

  std::vector<TopoDS_Vertex> vertices;
  vertices.reserve(segs.size());
  for (const auto& s : segs) {
    vertices.push_back(BRepBuilderAPI_MakeVertex(to_occ(segment_start(s))).Vertex());
  }

  BRepBuilderAPI_MakeWire wire;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const TopoDS_Vertex& v1 = vertices[i];
    const TopoDS_Vertex& v2 = vertices[(i + 1) % segs.size()];
    TopoDS_Edge edge;
    if (const auto* line = std::get_if<LineSegment>(&segs[i])) {
      if (core::distance(line->start, line->end) < settings.min_feature_size_mm()) {
        return Diagnostic::error("kernel.profile.degenerate_segment",
                                 fmt::format("Profile line {} is shorter than the minimum "
                                             "feature size ({} mm)",
                                             i, settings.min_feature_size_mm()));
      }
      BRepBuilderAPI_MakeEdge mk(v1, v2);
      if (!mk.IsDone()) {
        return Diagnostic::error("kernel.profile.bad_segment",
                                 fmt::format("Profile line {} could not be built", i));
      }
      edge = mk.Edge();
    } else {
      const auto& arc = std::get<ArcSegment>(segs[i]);
      GC_MakeArcOfCircle curve(to_occ(arc.start), to_occ(arc.through), to_occ(arc.end));
      if (!curve.IsDone()) {
        return Diagnostic::error(
            "kernel.profile.bad_segment",
            fmt::format("Profile arc {} could not be built; are its three points collinear?", i));
      }
      BRepBuilderAPI_MakeEdge mk(curve.Value(), v1, v2);
      if (!mk.IsDone()) {
        return Diagnostic::error("kernel.profile.bad_segment",
                                 fmt::format("Profile arc {} could not be built", i));
      }
      edge = mk.Edge();
    }
    wire.Add(edge);
    if (!wire.IsDone()) {
      return Diagnostic::error("kernel.profile.open_loop",
                               fmt::format("Profile segment {} does not connect", i));
    }
  }
  const TopoDS_Wire w = wire.Wire();
  if (!w.Closed() && !BRep_Tool::IsClosed(w)) {
    return Diagnostic::error("kernel.profile.open_loop", "A profile loop is not closed");
  }
  return w;
}

// Points along a loop (segment starts and arc midpoints), used for the
// orientation of holes and for side-of-axis checks.
std::vector<dvec3> loop_samples(const Loop& loop) {
  std::vector<dvec3> pts;
  for (const auto& s : loop.segments) {
    pts.push_back(segment_start(s));
    if (const auto* arc = std::get_if<ArcSegment>(&s)) pts.push_back(arc->through);
  }
  return pts;
}

// Twice the signed area of the polygon through `pts`, seen along `normal`.
double signed_area(const std::vector<dvec3>& pts, const dvec3& normal) {
  dvec3 sum{};
  for (std::size_t i = 0; i < pts.size(); ++i) {
    sum += core::cross(pts[i], pts[(i + 1) % pts.size()]);
  }
  return core::dot(sum, normal);
}

}  // namespace

Result<Face> make_face(const Loop& outer, std::span<const Loop> holes,
                       const ProductSettings& settings) {
  return detail::guarded<Result<Face>>("kernel.profile.failed", "Building a profile face", [&]()
                                                                                             -> Result<Face> {
    auto outer_wire = make_wire(outer, settings);
    if (!outer_wire) return outer_wire.error();

    BRepBuilderAPI_MakeFace mk(*outer_wire, /*OnlyPlane=*/true);
    if (!mk.IsDone()) {
      return Diagnostic::error("kernel.profile.not_planar",
                               "The outer profile loop does not lie in one plane");
    }
    const TopoDS_Face outer_face = mk.Face();
    const auto plane = occ::down_cast<Geom_Plane>(BRep_Tool::Surface(outer_face));
    if (plane.IsNull()) {
      return Diagnostic::error("kernel.profile.not_planar",
                               "The outer profile loop does not lie in one plane");
    }
    const gp_Pln pln = plane->Pln();
    dvec3 normal = from_occ(pln.Axis().Direction());
    if (outer_face.Orientation() == TopAbs_REVERSED) normal = -normal;

    for (std::size_t h = 0; h < holes.size(); ++h) {
      const auto samples = loop_samples(holes[h]);
      for (const auto& p : samples) {
        if (pln.Distance(to_occ(p)) > settings.linear_tolerance_mm) {
          return Diagnostic::error(
              "kernel.profile.not_planar",
              fmt::format("Hole {} does not lie in the plane of the outer loop", h));
        }
      }
      auto hole_wire = make_wire(holes[h], settings);
      if (!hole_wire) return hole_wire.error();
      BRepClass_FaceClassifier where(outer_face, to_occ(samples.front()),
                                     settings.linear_tolerance_mm);
      if (where.State() != TopAbs_IN) {
        return Diagnostic::error("kernel.profile.hole_outside",
                                 fmt::format("Hole {} is not inside the outer loop", h));
      }
      // A hole runs clockwise about the face normal.
      TopoDS_Wire w = *hole_wire;
      if (signed_area(samples, normal) > 0) w.Reverse();
      mk.Add(w);
    }

    auto face = Face::from(detail::wrap(mk.Face()));
    if (!face) return face;
    const Diagnostics problems = check_validity(*face);
    if (problems.has_errors()) {
      return Diagnostic::error(
          "kernel.profile.invalid",
          "The profile face is invalid; check that holes don't touch the outer loop or each "
          "other. " + problems[0].message);
    }
    return face;
  });
}

// --- primitive solids -----------------------------------------------------

namespace {

Result<Solid> finish_primitive(const TopoDS_Shape& shape, std::string_view what,
                               const ProductSettings& settings) {
  auto solid = detail::finish_solid(shape, "kernel.primitive.invalid", what);
  if (!solid) return solid;
  if (auto d = check_in_working_space(*solid, settings)) return *d;
  return solid;
}

std::optional<Diagnostic> check_axis(const Axis& axis) {
  if (!is_finite(axis.origin) || !is_finite(axis.direction) ||
      core::length(axis.direction) == 0.0) {
    return Diagnostic::error("kernel.primitive.bad_axis",
                             "The axis needs a finite origin and a non-zero direction");
  }
  return std::nullopt;
}

gp_Ax2 frame_for(const Axis& axis) {
  const dvec3 d = core::normalize(axis.direction);
  return gp_Ax2(to_occ(axis.origin), gp_Dir(d.x, d.y, d.z));
}

}  // namespace

Result<Solid> make_box(const dbox3& box, const ProductSettings& settings) {
  return detail::guarded<Result<Solid>>("kernel.primitive.failed", "Building a box",
                                        [&]() -> Result<Solid> {
    if (box.is_empty() || !is_finite(box.min) || !is_finite(box.max)) {
      return Diagnostic::error("kernel.primitive.invalid_size", "The box is empty or not finite");
    }
    const dvec3 size = box.size();
    for (int i = 0; i < 3; ++i) {
      if (auto d = check_length("kernel.primitive.invalid_size", "box size",
                                size[static_cast<std::size_t>(i)], settings)) {
        return *d;
      }
    }
    BRepPrimAPI_MakeBox mk(to_occ(box.min), to_occ(box.max));
    return finish_primitive(mk.Solid(), "box", settings);
  });
}

Result<Solid> make_cylinder(const Axis& axis, double radius, double height,
                            const ProductSettings& settings) {
  return detail::guarded<Result<Solid>>("kernel.primitive.failed", "Building a cylinder",
                                        [&]() -> Result<Solid> {
    if (auto d = check_axis(axis)) return *d;
    if (auto d = check_length("kernel.primitive.invalid_size", "cylinder radius", radius,
                              settings)) {
      return *d;
    }
    if (auto d = check_length("kernel.primitive.invalid_size", "cylinder height", height,
                              settings)) {
      return *d;
    }
    BRepPrimAPI_MakeCylinder mk(frame_for(axis), radius, height);
    return finish_primitive(mk.Solid(), "cylinder", settings);
  });
}

Result<Solid> make_sphere(const dvec3& center, double radius, const ProductSettings& settings) {
  return detail::guarded<Result<Solid>>("kernel.primitive.failed", "Building a sphere",
                                        [&]() -> Result<Solid> {
    if (!is_finite(center)) {
      return Diagnostic::error("kernel.primitive.invalid_size", "The centre is not finite");
    }
    if (auto d = check_length("kernel.primitive.invalid_size", "sphere radius", radius,
                              settings)) {
      return *d;
    }
    BRepPrimAPI_MakeSphere mk(to_occ(center), radius);
    return finish_primitive(mk.Solid(), "sphere", settings);
  });
}

Result<Solid> make_prism(const Face& profile, const dvec3& direction,
                         const ProductSettings& settings) {
  return detail::guarded<Result<Solid>>("kernel.primitive.failed", "Extruding a profile",
                                        [&]() -> Result<Solid> {
    if (profile.is_empty()) {
      return Diagnostic::error("kernel.primitive.empty_profile", "The profile is empty");
    }
    if (!is_finite(direction)) {
      return Diagnostic::error("kernel.primitive.invalid_size",
                               "The extrusion direction is not finite");
    }
    if (auto d = check_length("kernel.primitive.invalid_size", "extrusion depth",
                              core::length(direction), settings)) {
      return *d;
    }
    BRepPrimAPI_MakePrism mk(topo(profile), detail::to_occ_vec(direction));
    if (!mk.IsDone()) {
      return Diagnostic::error("kernel.primitive.failed", "The extrusion could not be built");
    }
    return finish_primitive(mk.Shape(), "extrusion", settings);
  });
}

Result<Solid> make_revolution(const Face& profile, const Axis& axis, double angle,
                              const ProductSettings& settings) {
  return detail::guarded<Result<Solid>>("kernel.primitive.failed", "Revolving a profile",
                                        [&]() -> Result<Solid> {
    if (profile.is_empty()) {
      return Diagnostic::error("kernel.primitive.empty_profile", "The profile is empty");
    }
    if (auto d = check_axis(axis)) return *d;
    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    if (!std::isfinite(angle) || angle <= 0.0 || angle > kTwoPi + 1e-12) {
      return Diagnostic::error("kernel.revolve.bad_angle",
                               "The revolve angle must be above 0 and at most one full turn");
    }

    // The axis must not pass through the profile's interior: every boundary
    // point must lie on one side of it (or on it).
    const TopoDS_Face& face = TopoDS::Face(topo(profile));
    const auto plane = occ::down_cast<Geom_Plane>(BRep_Tool::Surface(face));
    if (plane.IsNull()) {
      return Diagnostic::error("kernel.profile.not_planar", "The profile is not planar");
    }
    const dvec3 n = from_occ(plane->Pln().Axis().Direction());
    const dvec3 dir = core::normalize(axis.direction);
    const dvec3 side = core::cross(n, dir);  // in-plane, perpendicular to the axis
    const double tol = settings.linear_tolerance_mm;
    if (std::abs(core::dot(n, dir)) < 1e-9 && core::length(side) > 0) {
      double lo = 0.0;
      double hi = 0.0;
      for (const Edge& e : edges(profile)) {
        const auto& edge = TopoDS::Edge(topo(e));
        double first = 0.0;
        double last = 0.0;
        const auto curve = BRep_Tool::Curve(edge, first, last);
        if (curve.IsNull()) continue;
        constexpr int kSamples = 32;
        for (int i = 0; i <= kSamples; ++i) {
          const double t = first + (last - first) * i / kSamples;
          const double s = core::dot(from_occ(curve->Value(t)) - axis.origin, side);
          lo = std::min(lo, s);
          hi = std::max(hi, s);
        }
      }
      if (lo < -tol && hi > tol) {
        return Diagnostic::error("kernel.revolve.axis_crosses_profile",
                                 "The revolve axis passes through the profile; move the axis "
                                 "to one side of it");
      }
    }

    BRepPrimAPI_MakeRevol mk(face, gp_Ax1(to_occ(axis.origin), gp_Dir(dir.x, dir.y, dir.z)),
                             angle);
    if (!mk.IsDone()) {
      return Diagnostic::error("kernel.primitive.failed", "The revolve could not be built");
    }
    return finish_primitive(mk.Shape(), "revolve", settings);
  });
}

}  // namespace vs::kernel

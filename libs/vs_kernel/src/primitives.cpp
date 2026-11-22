#include "vs/kernel/primitives.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <BRepAdaptor_CompCurve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepTools.hxx>
#include <GeomFill_Trihedron.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <TopExp_Explorer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
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

// Builds a wire whose consecutive edges share vertices exactly: a closed
// loop, or (closed = false) an open chain.
Result<TopoDS_Wire> make_wire(const std::vector<ProfileSegment>& segs, bool closed,
                              const ProductSettings& settings) {
  const double tol = settings.linear_tolerance_mm;
  if (segs.empty()) {
    return Diagnostic::error("kernel.profile.empty_loop", "A profile loop has no segments");
  }
  const std::size_t joints = closed ? segs.size() : segs.size() - 1;
  for (std::size_t i = 0; i < joints; ++i) {
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
  vertices.reserve(segs.size() + 1);
  for (const auto& s : segs) {
    vertices.push_back(BRepBuilderAPI_MakeVertex(to_occ(segment_start(s))).Vertex());
  }
  if (!closed) {
    if (!is_finite(segment_end(segs.back()))) {
      return Diagnostic::error("kernel.profile.not_finite", "A path point is not a finite number");
    }
    vertices.push_back(BRepBuilderAPI_MakeVertex(to_occ(segment_end(segs.back()))).Vertex());
  }

  BRepBuilderAPI_MakeWire wire;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const TopoDS_Vertex& v1 = vertices[i];
    const TopoDS_Vertex& v2 = vertices[(i + 1) % vertices.size()];
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
  if (closed && !w.Closed() && !BRep_Tool::IsClosed(w)) {
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

// True when `loop` is all lines and turns the same way at every corner,
// once round in all, about `normal`: a strictly convex polygon, which
// can't cross itself. make_wire() has already refused sides shorter than
// the minimum feature size and gaps between them.
bool convex_polygon(const Loop& loop, const dvec3& normal) {
  const std::size_t n = loop.segments.size();
  if (n < 3) return false;
  std::vector<dvec3> sides;
  sides.reserve(n);
  for (const auto& s : loop.segments) {
    const auto* line = std::get_if<LineSegment>(&s);
    if (line == nullptr) return false;
    sides.push_back(line->end - line->start);
  }
  double sign = 0.0;
  double turning = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const dvec3& a = sides[i];
    const dvec3& b = sides[(i + 1) % n];
    const double sine = core::dot(core::cross(a, b), normal);
    const double cosine = core::dot(a, b);
    // Nearly straight or folded back corners go through the full check.
    if (std::abs(sine) <= 1e-9 * core::length(a) * core::length(b)) return false;
    if (sign == 0.0) sign = sine > 0.0 ? 1.0 : -1.0;
    if (sine * sign <= 0.0) return false;
    turning += std::atan2(sine * sign, cosine);
  }
  return std::abs(turning - 2.0 * std::numbers::pi) < 1e-6;
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
    auto outer_wire = make_wire(outer.segments, /*closed=*/true, settings);
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
      auto hole_wire = make_wire(holes[h].segments, /*closed=*/true, settings);
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
    // A strictly convex polygon without holes is valid by construction
    // (decision 0033): skip the general check, most of the face's cost.
    if (holes.empty() && convex_polygon(outer, normal)) return face;
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
    // A box of finite sides no shorter than the minimum feature size is
    // valid by construction (decision 0033): the general check was nine
    // tenths of its cost, and layouts make thousands.
    auto solid = Solid::from(detail::wrap(mk.Solid()));
    if (!solid) return solid;
    if (auto d = check_in_working_space(*solid, settings)) return *d;
    return solid;
  });
}

Result<Face> make_cylinder_face(const Axis& axis, const dvec3& x_direction, double radius,
                                double start_angle, double sweep, double start, double length,
                                const ProductSettings& settings) {
  return detail::guarded<Result<Face>>("kernel.primitive.failed", "Building a cylinder face",
                                       [&]() -> Result<Face> {
    if (auto d = check_axis(axis)) return *d;
    const dvec3 dir = core::normalize(axis.direction);
    const dvec3 x = x_direction - dir * core::dot(x_direction, dir);
    if (!is_finite(x_direction) || core::length(x) < 1e-12 ||
        !std::isfinite(start_angle) || !std::isfinite(start)) {
      return Diagnostic::error("kernel.primitive.bad_axis",
                               "The angle-0 direction must be finite and not along the axis");
    }
    if (auto d = check_length("kernel.primitive.invalid_size", "cylinder radius", radius, settings)) {
      return *d;
    }
    if (auto d = check_length("kernel.primitive.invalid_size", "face length", length, settings)) {
      return *d;
    }
    constexpr double kTurn = 2.0 * std::numbers::pi;
    if (!std::isfinite(sweep) || sweep > kTurn + 1e-12) {
      return Diagnostic::error("kernel.primitive.invalid_size",
                               "The face's sweep must be at most a full turn");
    }
    if (auto d = check_length("kernel.primitive.invalid_size", "face arc", sweep * radius,
                              settings)) {
      return *d;
    }
    const dvec3 xn = core::normalize(x);
    const gp_Ax3 frame(to_occ(axis.origin), gp_Dir(dir.x, dir.y, dir.z), gp_Dir(xn.x, xn.y, xn.z));
    const occ::handle<Geom_CylindricalSurface> surface = new Geom_CylindricalSurface(frame, radius);
    const double end_angle = start_angle + std::min(sweep, kTurn);
    BRepBuilderAPI_MakeFace mk(surface, start_angle, end_angle, start, start + length,
                               settings.linear_tolerance_mm);
    if (!mk.IsDone()) {
      return Diagnostic::error("kernel.primitive.invalid", "The cylinder face couldn't be built");
    }
    auto face = Face::from(detail::wrap(mk.Face()));
    if (!face) return face;
    const Diagnostics problems = check_validity(*face);
    if (problems.has_errors()) {
      return Diagnostic::error("kernel.primitive.invalid",
                               "The cylinder face is invalid. " + problems[0].message);
    }
    if (auto d = check_in_working_space(*face, settings)) return *d;
    return face;
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

Result<Solid> make_drafted_prism(const Face& profile, const dvec3& direction, double draft_angle,
                                 const ProductSettings& settings) {
  if (!std::isfinite(draft_angle) || std::abs(draft_angle) >= 89.0 * std::numbers::pi / 180.0) {
    return Diagnostic::error("kernel.draft.bad_angle",
                             "The draft angle must be a number between -89° and 89°");
  }
  if (draft_angle == 0.0) return make_prism(profile, direction, settings);
  return detail::guarded<Result<Solid>>("kernel.draft.failed", "Extruding with draft",
                                        [&]() -> Result<Solid> {
    if (profile.is_empty()) {
      return Diagnostic::error("kernel.primitive.empty_profile", "The profile is empty");
    }
    if (!is_finite(direction)) {
      return Diagnostic::error("kernel.primitive.invalid_size",
                               "The extrusion direction is not finite");
    }
    const double depth = core::length(direction);
    if (auto d = check_length("kernel.primitive.invalid_size", "extrusion depth", depth,
                              settings)) {
      return *d;
    }
    const TopoDS_Face& face = TopoDS::Face(topo(profile));
    const auto plane = occ::down_cast<Geom_Plane>(BRep_Tool::Surface(face));
    if (plane.IsNull()) {
      return Diagnostic::error("kernel.profile.not_planar", "The profile is not planar");
    }
    BRepPrimAPI_MakePrism prism(face, detail::to_occ_vec(direction));
    if (!prism.IsDone()) {
      return Diagnostic::error("kernel.primitive.failed", "The extrusion could not be built");
    }
    const TopoDS_Shape& first = prism.FirstShape();
    const TopoDS_Shape& last = prism.LastShape();
    const dvec3 d = direction / depth;
    const gp_Dir pull(d.x, d.y, d.z);
    const gp_Pln neutral = plane->Pln();

    BRepOffsetAPI_DraftAngle draft(prism.Shape());
    for (TopExp_Explorer ex(prism.Shape(), TopAbs_FACE); ex.More(); ex.Next()) {
      const TopoDS_Face& side = TopoDS::Face(ex.Current());
      if (side.IsSame(first) || side.IsSame(last)) continue;
      // OCCT's sign matches ours: a positive angle tilts a face's outward
      // normal towards the pull direction, so the section shrinks along it.
      draft.Add(side, pull, draft_angle, neutral);
      if (!draft.AddDone()) {
        return Diagnostic::error(
            "kernel.draft.failed",
            "A side face can't take the draft angle; try a smaller angle or a shorter extrusion");
      }
    }
    draft.Build();
    if (!draft.IsDone()) {
      return Diagnostic::error(
          "kernel.draft.failed",
          "The drafted extrusion could not be built; the section probably shrinks to nothing. "
          "Try a smaller angle or a shorter extrusion");
    }
    auto solid = detail::finish_solid(draft.Shape(), "kernel.draft.failed", "drafted extrusion");
    if (!solid) return solid;
    if (auto w = check_in_working_space(*solid, settings)) return *w;
    return solid;
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

// --- sweeps and lofts -------------------------------------------------------

namespace {

// Like finish_primitive, with the operation's own code for a bad result.
Result<Solid> finish_with(const TopoDS_Shape& shape, std::string_view invalid_code,
                          std::string_view what, const ProductSettings& settings) {
  auto solid = detail::finish_solid(shape, invalid_code, what);
  if (!solid) return solid;
  if (auto d = check_in_working_space(*solid, settings)) return *d;
  return solid;
}

}  // namespace

Result<Solid> make_sweep(const Face& profile, const Path& path, SweepOrientation orientation,
                         const ProductSettings& settings) {
  const std::string advice =
      "The sweep could not be built. The usual causes are a path that turns more tightly than "
      "the profile is wide (so the sweep runs into itself) or a profile that isn't at the "
      "path's start: try a larger bend radius, a smaller profile, or the other orientation mode";
  auto result = detail::guarded<Result<Solid>>("kernel.sweep.failed", "Sweeping a profile",
                                               [&]() -> Result<Solid> {
    if (profile.is_empty()) {
      return Diagnostic::error("kernel.primitive.empty_profile", "The profile is empty");
    }
    if (path.segments.empty()) {
      return Diagnostic::error("kernel.sweep.bad_path", "The sweep path has no segments");
    }
    const bool closed =
        path.segments.size() > 1 &&
        core::distance(segment_end(path.segments.back()), segment_start(path.segments.front())) <=
            settings.linear_tolerance_mm;
    auto wire = make_wire(path.segments, closed, settings);
    if (!wire) {
      return Diagnostic::error("kernel.sweep.bad_path",
                               fmt::format("The sweep path is broken: {}", wire.error().message));
    }
    const TopoDS_Face& face = TopoDS::Face(topo(profile));
    const auto plane = occ::down_cast<Geom_Plane>(BRep_Tool::Surface(face));
    if (plane.IsNull()) {
      return Diagnostic::error("kernel.profile.not_planar", "The profile is not planar");
    }
    BRepAdaptor_CompCurve curve(*wire);
    gp_Pnt p0;
    gp_Vec t0;
    curve.D1(curve.FirstParameter(), p0, t0);
    const dvec3 normal = from_occ(plane->Pln().Axis().Direction());
    if (t0.Magnitude() == 0.0 ||
        std::abs(core::dot(core::normalize(from_occ(t0)), normal)) < 1e-6) {
      return Diagnostic::error("kernel.sweep.tangent_in_profile",
                               "The path leaves the profile along the profile's own plane, so "
                               "the sweep would be flat; start the path across the profile");
    }
    const GeomFill_Trihedron mode = orientation == SweepOrientation::Fixed
                                        ? GeomFill_IsFixed
                                        : GeomFill_IsCorrectedFrenet;
    BRepOffsetAPI_MakePipe mk(*wire, face, mode, /*ForceApproxC1=*/false);
    mk.Build();
    if (!mk.IsDone()) return Diagnostic::error("kernel.sweep.failed", advice);
    return finish_with(mk.Shape(), "kernel.sweep.failed", "sweep", settings);
  });
  if (!result && result.error().code == "kernel.sweep.failed" &&
      result.error().message != advice) {
    return Diagnostic::error("kernel.sweep.failed",
                             fmt::format("{} ({})", advice, result.error().message));
  }
  return result;
}

Result<Solid> make_loft(std::span<const Face> sections, bool ruled,
                        const ProductSettings& settings) {
  if (sections.size() < 2) {
    return Diagnostic::error("kernel.loft.too_few_sections",
                             "A loft needs at least two sections");
  }
  return detail::guarded<Result<Solid>>("kernel.loft.failed", "Lofting", [&]() -> Result<Solid> {
    BRepOffsetAPI_ThruSections mk(/*isSolid=*/true, ruled, settings.linear_tolerance_mm);
    for (std::size_t i = 0; i < sections.size(); ++i) {
      if (sections[i].is_empty()) {
        return Diagnostic::error("kernel.primitive.empty_profile",
                                 fmt::format("Loft section {} is empty", i));
      }
      const TopoDS_Face& face = TopoDS::Face(topo(sections[i]));
      int wires = 0;
      for (TopExp_Explorer ex(face, TopAbs_WIRE); ex.More(); ex.Next()) ++wires;
      if (wires != 1) {
        return Diagnostic::error("kernel.loft.section_has_holes",
                                 fmt::format("Loft section {} has holes; a loft uses only outer "
                                             "boundaries, so cut the holes afterwards",
                                             i));
      }
      mk.AddWire(BRepTools::OuterWire(face));
    }
    mk.CheckCompatibility(true);
    mk.Build();
    if (!mk.IsDone()) {
      return Diagnostic::error(
          "kernel.loft.failed",
          "The loft could not be built. Sections whose corners can't be matched up (very "
          "different numbers of edges) or that cross each other are the usual cause: give the "
          "sections the same number of edges, or add a section in between");
    }
    return finish_with(mk.Shape(), "kernel.loft.failed", "loft", settings);
  });
}

}  // namespace vs::kernel

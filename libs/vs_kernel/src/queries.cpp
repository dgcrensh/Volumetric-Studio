#include "vs/kernel/queries.hpp"

#include <cmath>

#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <BRepBndLib.hxx>
#include <ElSLib.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <NCollection_IndexedMap.hxx>
#include <TopExp.hxx>
#include <TopTools_ShapeMapHasher.hxx>

#include "occt.hpp"

namespace vs::kernel {

using detail::from_occ;
using detail::topo;

namespace {

using ShapeMap = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;

template <typename T>
std::vector<T> sub_shapes(const Shape& shape, TopAbs_ShapeEnum type) {
  std::vector<T> out;
  if (shape.is_empty()) return out;
  ShapeMap map;
  TopExp::MapShapes(topo(shape), type, map);
  out.reserve(static_cast<std::size_t>(map.Extent()));
  for (int i = 1; i <= map.Extent(); ++i) out.push_back(detail::wrap<T>(map(i)));
  return out;
}

}  // namespace

Result<MassProperties> mass_properties(const Solid& solid) {
  return detail::guarded<Result<MassProperties>>(
      "kernel.mass.failed", "Computing mass properties", [&]() -> Result<MassProperties> {
        if (solid.is_empty()) {
          return Diagnostic::error("kernel.mass.empty", "The solid is empty");
        }
        GProp_GProps props;
        // The adaptive integration (with a relative error bound) is exact
        // enough for B-spline faces too; the default fixed-order one is not.
        BRepGProp::VolumeProperties(topo(solid), props, 1e-10);
        MassProperties out;
        out.volume = props.Mass();
        if (!(out.volume > 0.0)) {
          return Diagnostic::error("kernel.mass.empty",
                                   fmt::format("The solid has no positive volume ({} mm³)",
                                               out.volume));
        }
        out.center_of_mass = from_occ(props.CentreOfMass());
        return out;
      });
}

double surface_area(const Shape& shape) {
  if (shape.is_empty()) return 0.0;
  GProp_GProps props;
  BRepGProp::SurfaceProperties(topo(shape), props);
  return props.Mass();
}

double length(const Edge& edge) {
  if (edge.is_empty()) return 0.0;
  GProp_GProps props;
  BRepGProp::LinearProperties(topo(edge), props);
  return props.Mass();
}

dbox3 bounding_box(const Shape& shape) {
  if (shape.is_empty()) return dbox3::empty();
  Bnd_Box box;
  BRepBndLib::AddOptimal(topo(shape), box, /*useTriangulation=*/false,
                         /*useShapeTolerance=*/false);
  if (box.IsVoid()) return dbox3::empty();
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  box.Get(x0, y0, z0, x1, y1, z1);
  return dbox3::from_min_max({x0, y0, z0}, {x1, y1, z1});
}

Result<DistanceResult> min_distance(const Shape& a, const Shape& b) {
  if (a.is_empty() || b.is_empty()) {
    return Diagnostic::error("kernel.distance.empty_input",
                             "Distance needs two non-empty shapes");
  }
  return detail::guarded<Result<DistanceResult>>(
      "kernel.distance.failed", "Measuring distance", [&]() -> Result<DistanceResult> {
        BRepExtrema_DistShapeShape dist(topo(a), topo(b));
        if (!dist.IsDone() || dist.NbSolution() < 1) {
          return Diagnostic::error("kernel.distance.failed",
                                   "The kernel could not measure the distance");
        }
        DistanceResult out;
        // When one shape lies wholly inside a solid, the closest points are
        // on the boundaries, but the shapes overlap: the distance is 0.
        out.distance = dist.InnerSolution() ? 0.0 : dist.Value();
        out.point_on_a = from_occ(dist.PointOnShape1(1));
        out.point_on_b = from_occ(dist.PointOnShape2(1));
        return out;
      });
}

PointClass classify(const Solid& solid, const dvec3& point, const ProductSettings& settings) {
  bool on = false;
  for (const Solid& s : solids(solid)) {
    BRepClass3d_SolidClassifier classifier(topo(s));
    classifier.Perform(detail::to_occ(point), settings.linear_tolerance_mm);
    switch (classifier.State()) {
      case TopAbs_IN: return PointClass::Inside;
      case TopAbs_ON: on = true; break;
      default: break;
    }
  }
  return on ? PointClass::OnBoundary : PointClass::Outside;
}

std::vector<Face> faces(const Shape& shape) { return sub_shapes<Face>(shape, TopAbs_FACE); }

std::optional<Plane> face_plane(const Face& face) {
  if (face.is_empty()) return std::nullopt;
  try {
    const TopoDS_Face& f = TopoDS::Face(topo(face));
    BRepAdaptor_Surface surface(f, /*theRestriction=*/false);
    if (surface.GetType() != GeomAbs_Plane) return std::nullopt;
    const gp_Pln pln = surface.Plane();
    dvec3 normal = detail::from_occ(pln.Axis().Direction());
    if (f.Orientation() == TopAbs_REVERSED) normal = -normal;
    GProp_GProps props;
    BRepGProp::SurfaceProperties(f, props);
    // Project the centre of area onto the plane, in case integration leaves
    // it a hair off.
    gp_Pnt c = props.CentreOfMass();
    const gp_Pnt on = ElSLib::Value(0, 0, pln);
    const double off = gp_Vec(on, c).Dot(gp_Vec(pln.Axis().Direction()));
    c.Translate(gp_Vec(pln.Axis().Direction()) * -off);
    return Plane{detail::from_occ(c), normal};
  } catch (...) {
    return std::nullopt;
  }
}
std::optional<Cylinder> face_cylinder(const Face& face) {
  if (face.is_empty()) return std::nullopt;
  try {
    const TopoDS_Face& f = TopoDS::Face(topo(face));
    BRepAdaptor_Surface surface(f, /*theRestriction=*/false);
    if (surface.GetType() != GeomAbs_Cylinder) return std::nullopt;
    const gp_Cylinder cyl = surface.Cylinder();
    // A right-handed cylinder's natural normal points away from the axis.
    bool outward = cyl.Direct();
    if (f.Orientation() == TopAbs_REVERSED) outward = !outward;
    return Cylinder{Axis{from_occ(cyl.Location()), from_occ(cyl.Axis().Direction())},
                    cyl.Radius(), !outward};
  } catch (...) {
    return std::nullopt;
  }
}

std::vector<Edge> edges(const Shape& shape) { return sub_shapes<Edge>(shape, TopAbs_EDGE); }

bool is_seam(const Edge& edge, const Face& face) {
  if (edge.is_empty() || face.is_empty()) return false;
  try {
    return BRep_Tool::IsClosed(TopoDS::Edge(topo(edge)), TopoDS::Face(topo(face)));
  } catch (...) {
    return false;
  }
}
std::vector<Solid> solids(const Shape& shape) { return sub_shapes<Solid>(shape, TopAbs_SOLID); }

std::size_t vertex_count(const Shape& shape) {
  if (shape.is_empty()) return 0;
  ShapeMap map;
  TopExp::MapShapes(topo(shape), TopAbs_VERTEX, map);
  return static_cast<std::size_t>(map.Extent());
}

}  // namespace vs::kernel

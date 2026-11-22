#include "vs/kernel/queries.hpp"

#include <algorithm>
#include <cmath>

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopoDS_Wire.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Circ.hxx>
#include <gp_Lin.hxx>
#include <BRepBndLib.hxx>
#include <ElSLib.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>
#include <TopLoc_Location.hxx>
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

std::optional<std::pair<double, double>> extent(const Shape& shape, const dvec3& direction) {
  if (shape.is_empty()) return std::nullopt;
  const double len = core::length(direction);
  if (!std::isfinite(len) || len == 0.0) return std::nullopt;
  const dvec3 d = direction / len;
  // Along a principal axis, the box itself.
  for (int axis = 0; axis < 3; ++axis) {
    const double a = d[static_cast<std::size_t>(axis)];
    const bool principal = std::abs(a) == 1.0;
    if (!principal) continue;
    const dbox3 box = bounding_box(shape);
    if (box.is_empty()) return std::nullopt;
    const double lo = box.min[static_cast<std::size_t>(axis)];
    const double hi = box.max[static_cast<std::size_t>(axis)];
    return a > 0 ? std::pair{lo * len, hi * len} : std::pair{-hi * len, -lo * len};
  }
  // Otherwise turn the direction onto +x (a location only: the geometry is
  // shared) and take the box's x range.
  gp_Trsf turn;
  turn.SetRotation(gp_Quaternion(gp_Vec(d.x, d.y, d.z), gp_Vec(1, 0, 0)));
  const TopoDS_Shape moved = topo(shape).Moved(TopLoc_Location(turn));
  Bnd_Box box;
  BRepBndLib::AddOptimal(moved, box, /*useTriangulation=*/false, /*useShapeTolerance=*/false);
  if (box.IsVoid()) return std::nullopt;
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  box.Get(x0, y0, z0, x1, y1, z1);
  return std::pair{x0 * len, x1 * len};
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

struct SolidClassifier::Impl {
  std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> lumps;
  double tolerance = 0.0;
};

SolidClassifier::~SolidClassifier() = default;
SolidClassifier::SolidClassifier(SolidClassifier&&) noexcept = default;
SolidClassifier& SolidClassifier::operator=(SolidClassifier&&) noexcept = default;

SolidClassifier::SolidClassifier(const Solid& solid, const ProductSettings& settings)
    : impl_(std::make_unique<Impl>()) {
  impl_->tolerance = settings.linear_tolerance_mm;
  for (const Solid& s : solids(solid)) {
    impl_->lumps.push_back(std::make_unique<BRepClass3d_SolidClassifier>(topo(s)));
  }
}

PointClass SolidClassifier::classify(const dvec3& point) const {
  bool on = false;
  for (auto& lump : impl_->lumps) {
    lump->Perform(detail::to_occ(point), impl_->tolerance);
    switch (lump->State()) {
      case TopAbs_IN: return PointClass::Inside;
      case TopAbs_ON: on = true; break;
      default: break;
    }
  }
  return on ? PointClass::OnBoundary : PointClass::Outside;
}

Result<double> distance_to(const Shape& shape, const dvec3& point) {
  if (shape.is_empty()) {
    return Diagnostic::error("kernel.distance.empty_input", "Distance needs a non-empty shape");
  }
  return detail::guarded<Result<double>>(
      "kernel.distance.failed", "Measuring distance", [&]() -> Result<double> {
        const TopoDS_Shape vertex = BRepBuilderAPI_MakeVertex(detail::to_occ(point)).Vertex();
        BRepExtrema_DistShapeShape dist(topo(shape), vertex);
        if (!dist.IsDone() || dist.NbSolution() < 1) {
          return Diagnostic::error("kernel.distance.failed",
                                   "The kernel could not measure the distance");
        }
        return dist.InnerSolution() ? 0.0 : dist.Value();
      });
}

std::vector<Face> faces(const Shape& shape) { return sub_shapes<Face>(shape, TopAbs_FACE); }

std::optional<Plane> face_plane(const Face& face) {
  if (face.is_empty()) return std::nullopt;
  try {
    const TopoDS_Face& f = TopoDS::Face(topo(face));
    BRepAdaptor_Surface surface(f, /*theRestriction=*/false);
    if (surface.GetType() != GeomAbs_Plane) return std::nullopt;
    const gp_Pln pln = surface.Plane();
    // The surface's natural normal is X x Y, against the axis for a
    // left-handed plane (revolutions make those).
    dvec3 normal = detail::from_occ(pln.Axis().Direction());
    if (!pln.Direct()) normal = -normal;
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
std::optional<Plane> face_surface_plane(const Face& face) {
  if (face.is_empty()) return std::nullopt;
  try {
    const TopoDS_Face& f = TopoDS::Face(topo(face));
    BRepAdaptor_Surface surface(f, /*theRestriction=*/false);
    if (surface.GetType() != GeomAbs_Plane) return std::nullopt;
    const gp_Pln pln = surface.Plane();
    dvec3 normal = detail::from_occ(pln.Axis().Direction());
    if (!pln.Direct()) normal = -normal;
    if (f.Orientation() == TopAbs_REVERSED) normal = -normal;
    return Plane{detail::from_occ(pln.Location()), normal};
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

namespace {

// The edge in its curve's parameter direction; face_loops() turns it to
// follow the loop.
LoopEdge loop_edge(const TopoDS_Edge& edge) {
  LoopEdge out;
  BRepAdaptor_Curve curve(edge);
  const double first = curve.FirstParameter();
  const double last = curve.LastParameter();
  out.start = detail::from_occ(curve.Value(first));
  out.end = detail::from_occ(curve.Value(last));
  out.length = GCPnts_AbscissaPoint::Length(curve);
  switch (curve.GetType()) {
    case GeomAbs_Line:
      out.kind = LoopEdge::Kind::Line;
      break;
    case GeomAbs_Circle: {
      const gp_Circ circ = curve.Circle();
      out.kind = LoopEdge::Kind::Circle;
      out.centre = detail::from_occ(circ.Location());
      out.radius = circ.Radius();
      out.axis = detail::from_occ(circ.Axis().Direction());
      out.sweep = last - first;
      break;
    }
    default:
      out.kind = LoopEdge::Kind::Other;
      break;
  }
  return out;
}

void reverse_edge(LoopEdge& e) {
  std::swap(e.start, e.end);
  e.axis = -e.axis;
}

// Turns edges so each starts where the previous one ends.
void chain(std::vector<LoopEdge>& edges) {
  if (edges.size() < 2) return;
  const auto d = [](const dvec3& a, const dvec3& b) { return core::length(a - b); };
  // The first edge's end must meet the second edge.
  const LoopEdge& second = edges[1];
  if (std::min(d(edges[0].start, second.start), d(edges[0].start, second.end)) <
      std::min(d(edges[0].end, second.start), d(edges[0].end, second.end))) {
    reverse_edge(edges[0]);
  }
  for (std::size_t i = 1; i < edges.size(); ++i) {
    const dvec3& at = edges[i - 1].end;
    if (d(edges[i].end, at) < d(edges[i].start, at)) reverse_edge(edges[i]);
  }
}

}  // namespace

std::vector<FaceLoop> face_loops(const Face& face) {
  std::vector<FaceLoop> loops;
  if (face.is_empty()) return loops;
  try {
    const TopoDS_Face& f = TopoDS::Face(topo(face));
    const TopoDS_Wire outer = BRepTools::OuterWire(f);
    std::vector<TopoDS_Wire> wires;
    if (!outer.IsNull()) wires.push_back(outer);
    for (TopExp_Explorer it(f, TopAbs_WIRE); it.More(); it.Next()) {
      const TopoDS_Wire& w = TopoDS::Wire(it.Current());
      if (!w.IsSame(outer)) wires.push_back(w);
    }
    for (const TopoDS_Wire& w : wires) {
      FaceLoop loop;
      for (BRepTools_WireExplorer ex(w, f); ex.More(); ex.Next()) {
        const TopoDS_Edge& e = ex.Current();
        if (BRep_Tool::Degenerated(e) || BRep_Tool::IsClosed(e, f)) continue;
        loop.edges.push_back(loop_edge(e));
      }
      chain(loop.edges);
      if (!loop.edges.empty()) loops.push_back(std::move(loop));
    }
  } catch (...) {
    loops.clear();
  }
  return loops;
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

std::vector<dvec3> vertex_points(const Shape& shape) {
  std::vector<dvec3> out;
  if (shape.is_empty()) return out;
  ShapeMap map;
  TopExp::MapShapes(topo(shape), TopAbs_VERTEX, map);
  out.reserve(static_cast<std::size_t>(map.Extent()));
  for (int i = 1; i <= map.Extent(); ++i) {
    out.push_back(from_occ(BRep_Tool::Pnt(TopoDS::Vertex(map(i)))));
  }
  return out;
}

}  // namespace vs::kernel

#include "vs/kernel/tessellate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <ElCLib.hxx>
#include <Extrema_ExtPC.hxx>
#include <Geom2d_Curve.hxx>
#include <IMeshTools_Parameters.hxx>
#include <NCollection_IndexedMap.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pnt2d.hxx>

#include "occt.hpp"
#include "vs/kernel/queries.hpp"

namespace vs::kernel {

using detail::from_occ;
using detail::to_occ;
using detail::topo;

namespace {

using ShapeMap = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;

Diagnostic failed(std::string message) {
  return Diagnostic::error("kernel.tessellate.failed", std::move(message));
}

// Merges faces on one surface and edges on one curve; the shape as it was
// when that fails.
TopoDS_Shape unified(const TopoDS_Shape& shape) {
  try {
    ShapeUpgrade_UnifySameDomain unify(shape, true, true, false);
    unify.Build();
    const TopoDS_Shape& out = unify.Shape();
    if (!out.IsNull()) return out;
  } catch (...) {
  }
  return shape;
}

}  // namespace

core::Result<Tessellation> tessellate(const Shape& shape, const TessellationParams& params,
                                      const core::ProductSettings& settings) {
  (void)settings;
  if (!(std::isfinite(params.deflection) && params.deflection > 0 &&
        std::isfinite(params.angle) && params.angle > 0)) {
    return Diagnostic::error("kernel.tessellate.params",
                             "The deflection and angle must be positive");
  }
  if (shape.is_empty()) return Diagnostic::error("kernel.tessellate.empty", "The shape is empty");
  return detail::guarded<core::Result<Tessellation>>(
      "kernel.tessellate.failed", "Tessellation", [&]() -> core::Result<Tessellation> {
        BRepBuilderAPI_Copy copy(unified(topo(shape)), true, false);
        const TopoDS_Shape s = copy.Shape();

        ShapeMap vertex_map, edge_map, face_map;
        TopExp::MapShapes(s, TopAbs_VERTEX, vertex_map);
        TopExp::MapShapes(s, TopAbs_EDGE, edge_map);
        // Faces as the solid holds them (orientation included).
        for (TopExp_Explorer it(s, TopAbs_FACE); it.More(); it.Next()) face_map.Add(it.Current());
        if (face_map.IsEmpty()) {
          return Diagnostic::error("kernel.tessellate.empty", "The shape has no faces");
        }

        IMeshTools_Parameters mp;
        mp.Deflection = params.deflection;
        mp.Angle = params.angle;
        mp.DeflectionInterior = params.deflection;
        mp.AngleInterior = params.angle;
        mp.InParallel = false;
        mp.Relative = false;
        BRepMesh_IncrementalMesh mesher(s, mp);
        if (!mesher.IsDone()) return failed("BRepMesh did not finish");

        Tessellation out;
        out.vertices.reserve(static_cast<std::size_t>(vertex_map.Extent()));
        for (int i = 1; i <= vertex_map.Extent(); ++i) {
          out.vertices.push_back(from_occ(BRep_Tool::Pnt(TopoDS::Vertex(vertex_map(i)))));
        }
        const auto vertex_index = [&](const TopoDS_Vertex& v) {
          return static_cast<std::uint32_t>(vertex_map.FindIndex(v) - 1);
        };

        // Edges: ends and BRepMesh's division (from the first face's polygon).
        out.edges.resize(static_cast<std::size_t>(edge_map.Extent()));
        std::vector<bool> divided(out.edges.size(), false);
        for (int i = 1; i <= edge_map.Extent(); ++i) {
          const TopoDS_Edge e = TopoDS::Edge(edge_map(i).Oriented(TopAbs_FORWARD));
          TessEdge& te = out.edges[static_cast<std::size_t>(i - 1)];
          te.edge = detail::wrap<Edge>(e);
          TopoDS_Vertex v0, v1;
          TopExp::Vertices(e, v0, v1);
          if (v0.IsNull() || v1.IsNull()) return failed("An edge has no vertices");
          te.first_vertex = vertex_index(v0);
          te.last_vertex = vertex_index(v1);
          te.degenerate = BRep_Tool::Degenerated(e);
        }
        const auto take_division = [&](std::uint32_t ei, const Poly_PolygonOnTriangulation& poly) {
          if (divided[ei]) return true;
          if (!poly.HasParameters() || poly.NbNodes() < 2) return false;
          TessEdge& te = out.edges[ei];
          const auto& prm = poly.Parameters()->Array1();
          std::vector<double> ps;
          for (int k = prm.Lower(); k <= prm.Upper(); ++k) ps.push_back(prm(k));
          if (ps.front() > ps.back()) std::reverse(ps.begin(), ps.end());
          te.params = ps;
          te.points.resize(ps.size());
          const TopoDS_Edge& e = TopoDS::Edge(topo(te.edge));
          std::optional<BRepAdaptor_Curve> curve;
          if (!te.degenerate) curve.emplace(e);
          for (std::size_t k = 0; k < ps.size(); ++k) {
            if (k == 0) {
              te.points[k] = out.vertices[te.first_vertex];
            } else if (k + 1 == ps.size()) {
              te.points[k] = out.vertices[te.last_vertex];
            } else if (te.degenerate) {
              te.points[k] = out.vertices[te.first_vertex];
            } else {
              te.points[k] = from_occ(curve->Value(ps[k]));
            }
          }
          divided[ei] = true;
          return true;
        };

        out.faces.reserve(static_cast<std::size_t>(face_map.Extent()));
        for (int fi = 1; fi <= face_map.Extent(); ++fi) {
          const TopoDS_Face f = TopoDS::Face(face_map(fi));
          TessFace tf;
          tf.face = detail::wrap<Face>(f);
          tf.plane = face_plane(tf.face);
          tf.uv_outward = f.Orientation() != TopAbs_REVERSED;

          TopLoc_Location loc;
          const occ::handle<Poly_Triangulation>& tri = BRep_Tool::Triangulation(f, loc);
          if (tri.IsNull() || tri->NbTriangles() == 0 || !tri->HasUVNodes()) {
            return failed("BRepMesh left a face without triangles");
          }
          const gp_Trsf trsf = loc.Transformation();
          const int n = tri->NbNodes();
          tf.points.resize(static_cast<std::size_t>(n));
          tf.uv.resize(static_cast<std::size_t>(n));
          tf.nodes.assign(static_cast<std::size_t>(n), TessNode{});
          for (int k = 1; k <= n; ++k) {
            tf.points[static_cast<std::size_t>(k - 1)] = from_occ(tri->Node(k).Transformed(trsf));
            const gp_Pnt2d uv = tri->UVNode(k);
            tf.uv[static_cast<std::size_t>(k - 1)] = {uv.X(), uv.Y()};
          }
          tf.triangles.reserve(static_cast<std::size_t>(tri->NbTriangles()));
          for (int t = 1; t <= tri->NbTriangles(); ++t) {
            int a = 0, b = 0, c = 0;
            tri->Triangle(t).Get(a, b, c);
            if (!tf.uv_outward) std::swap(b, c);
            tf.triangles.push_back({static_cast<std::uint32_t>(a - 1),
                                    static_cast<std::uint32_t>(b - 1),
                                    static_cast<std::uint32_t>(c - 1)});
          }

          // Wires, outer first, and where the triangulation's points sit.
          const TopoDS_Wire outer = BRepTools::OuterWire(f);
          std::vector<TopoDS_Wire> wires;
          if (!outer.IsNull()) wires.push_back(outer);
          for (TopExp_Explorer it(f, TopAbs_WIRE); it.More(); it.Next()) {
            const TopoDS_Wire& w = TopoDS::Wire(it.Current());
            if (outer.IsNull() || !w.IsSame(outer)) wires.push_back(w);
          }
          for (const TopoDS_Wire& w : wires) {
            std::vector<TessEdgeUse> uses;
            for (BRepTools_WireExplorer ex(w, f); ex.More(); ex.Next()) {
              const TopoDS_Edge& e = ex.Current();
              const int index = edge_map.FindIndex(e);
              if (index <= 0) return failed("A face's edge is not in the shape");
              const auto ei = static_cast<std::uint32_t>(index - 1);
              TessEdgeUse use;
              use.edge = ei;
              use.reversed = e.Orientation() == TopAbs_REVERSED;
              use.oriented = detail::wrap<Edge>(e);
              uses.push_back(use);

              const occ::handle<Poly_PolygonOnTriangulation>& poly =
                  BRep_Tool::PolygonOnTriangulation(e, tri, loc);
              if (poly.IsNull()) return failed("BRepMesh left an edge undivided on a face");
              if (!take_division(ei, *poly)) return failed("An edge's division has no parameters");
              const TessEdge& te = out.edges[ei];
              const auto& nodes = poly->Nodes();
              const auto& prm = poly->Parameters()->Array1();
              if (static_cast<std::size_t>(nodes.Length()) != te.params.size()) {
                return failed("An edge is divided differently on two faces");
              }
              const bool ascending = prm(prm.Lower()) <= prm(prm.Upper());
              const std::size_t count = te.params.size();
              for (int k = 0; k < nodes.Length(); ++k) {
                const auto node = static_cast<std::size_t>(nodes(nodes.Lower() + k) - 1);
                if (node >= tf.nodes.size()) return failed("A polygon node is out of range");
                const std::size_t j = ascending ? static_cast<std::size_t>(k)
                                                : count - 1 - static_cast<std::size_t>(k);
                TessNode tn;
                if (j == 0 || te.degenerate) {
                  tn = {TessNode::Kind::Vertex, te.first_vertex, 0};
                } else if (j + 1 == count) {
                  tn = {TessNode::Kind::Vertex, te.last_vertex, 0};
                } else {
                  tn = {TessNode::Kind::Edge, ei, static_cast<std::uint32_t>(j)};
                }
                tf.nodes[node] = tn;
              }
            }
            if (!uses.empty()) tf.wires.push_back(std::move(uses));
          }

          // The surface's speed at the middle of the face's (u, v) range.
          double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
          BRepTools::UVBounds(f, u0, u1, v0, v1);
          BRepAdaptor_Surface surface(f, false);
          gp_Pnt p;
          gp_Vec du, dv;
          surface.D1(0.5 * (u0 + u1), 0.5 * (v0 + v1), p, du, dv);
          const double su = du.Magnitude(), sv = dv.Magnitude();
          tf.uv_scale = {su > 1e-9 ? su : 1.0, sv > 1e-9 ? sv : 1.0};
          out.faces.push_back(std::move(tf));
        }
        for (std::size_t e = 0; e < out.edges.size(); ++e) {
          if (!divided[e]) return failed("An edge belongs to no face");
        }
        return out;
      });
}

std::optional<double> edge_parameter(const Edge& edge, const dvec3& point, double tolerance) {
  if (edge.is_empty()) return std::nullopt;
  try {
    const TopoDS_Edge& e = TopoDS::Edge(topo(edge));
    if (BRep_Tool::Degenerated(e)) return std::nullopt;
    BRepAdaptor_Curve curve(e);
    const double first = curve.FirstParameter(), last = curve.LastParameter();
    const gp_Pnt p = to_occ(point);
    // The ends first: a point at a vertex gets the vertex's parameter.
    if (curve.Value(first).Distance(p) <= tolerance) return first;
    if (curve.Value(last).Distance(p) <= tolerance) return last;
    double t = 0;
    switch (curve.GetType()) {
      case GeomAbs_Line:
        t = ElCLib::Parameter(curve.Line(), p);
        break;
      case GeomAbs_Circle: {
        t = ElCLib::Parameter(curve.Circle(), p);
        // Into [first, first + 2π).
        const double period = 2 * std::numbers::pi;
        t = first + std::fmod(std::fmod(t - first, period) + period, period);
        break;
      }
      default: {
        Extrema_ExtPC ext(p, curve, first, last);
        if (!ext.IsDone() || ext.NbExt() == 0) return std::nullopt;
        int best = 1;
        for (int i = 2; i <= ext.NbExt(); ++i) {
          if (ext.SquareDistance(i) < ext.SquareDistance(best)) best = i;
        }
        t = ext.Point(best).Parameter();
        break;
      }
    }
    if (t < first || t > last) return std::nullopt;
    if (curve.Value(t).Distance(p) > tolerance) return std::nullopt;
    return t;
  } catch (...) {
    return std::nullopt;
  }
}

dvec3 edge_point(const Edge& edge, double t) {
  const TopoDS_Edge& e = TopoDS::Edge(topo(edge));
  if (BRep_Tool::Degenerated(e)) {
    TopoDS_Vertex v0, v1;
    TopExp::Vertices(e, v0, v1);
    return from_occ(BRep_Tool::Pnt(v0));
  }
  return from_occ(BRepAdaptor_Curve(e).Value(t));
}

dvec2 edge_uv(const Face& face, const Edge& oriented, double t) {
  const TopoDS_Edge& e = TopoDS::Edge(topo(oriented));
  const TopoDS_Face& f = TopoDS::Face(topo(face));
  double first = 0, last = 0;
  const occ::handle<Geom2d_Curve> c = BRep_Tool::CurveOnSurface(e, f, first, last);
  if (c.IsNull()) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    return {nan, nan};
  }
  const gp_Pnt2d p = c->Value(t);
  return {p.X(), p.Y()};
}

}  // namespace vs::kernel

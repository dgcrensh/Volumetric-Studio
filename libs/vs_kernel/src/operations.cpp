#include "vs/kernel/operations.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRep_Builder.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Cylinder.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Sphere.hxx>
#include <gp_Mat.hxx>
#include <gp_Trsf.hxx>
#include <gp_XYZ.hxx>

#include "occt.hpp"

namespace vs::kernel {

using detail::topo;

namespace {

// The solids in `shape`, gathered into one solid or a compound of them.
// Returns a null shape when there are none.
TopoDS_Shape collect_solids(const TopoDS_Shape& shape) {
  std::vector<TopoDS_Shape> found;
  for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next()) found.push_back(ex.Current());
  if (found.empty()) return {};
  if (found.size() == 1) return found.front();
  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  for (const auto& s : found) builder.Add(compound, s);
  return compound;
}

enum class BooleanKind { Unite, Cut, Common };

std::string_view name_of(BooleanKind kind) {
  switch (kind) {
    case BooleanKind::Unite: return "union";
    case BooleanKind::Cut: return "cut";
    case BooleanKind::Common: return "intersection";
  }
  return "boolean";
}

template <typename Op>
Result<Solid> run_boolean(const Solid& a, std::span<const Solid> tools_in, BooleanKind kind,
                          const BooleanOptions& options) {
  const std::string_view name = name_of(kind);
  const bool empty_tool = std::any_of(tools_in.begin(), tools_in.end(),
                                      [](const Solid& t) { return t.is_empty(); });
  if (a.is_empty() || tools_in.empty() || empty_tool) {
    return Diagnostic::error("kernel.boolean.empty_input",
                             fmt::format("The {} needs non-empty solids", name));
  }
  if (!std::isfinite(options.fuzzy_mm) || options.fuzzy_mm < 0.0) {
    return Diagnostic::error("kernel.boolean.bad_fuzzy",
                             "The boolean fuzzy distance must be zero or positive");
  }
  return detail::guarded<Result<Solid>>(
      "kernel.boolean.failed", fmt::format("The {}", name), [&]() -> Result<Solid> {
        NCollection_List<TopoDS_Shape> arguments;
        NCollection_List<TopoDS_Shape> tools;
        arguments.Append(topo(a));
        for (const Solid& t : tools_in) tools.Append(topo(t));
        Op op;
        op.SetArguments(arguments);
        op.SetTools(tools);
        // Never change the inputs in place (they are shared, immutable
        // handles): where the operation has to widen a tolerance or split
        // an edge of an argument, it works on a copy (plan H1).
        op.SetNonDestructive(true);
        if (options.fuzzy_mm > 0.0) op.SetFuzzyValue(options.fuzzy_mm);
        op.Build();
        if (op.HasErrors()) {
          std::ostringstream detail_text;
          op.DumpErrors(detail_text);
          return Diagnostic::error(
              "kernel.boolean.failed",
              fmt::format("The {} of the two solids failed in the kernel. Faces that nearly "
                          "coincide (closer than the tolerance but not touching) are the usual "
                          "cause: make them coincide exactly or move them apart. ({})",
                          name, detail_text.str()));
        }
        // Merge faces and edges that the operation split but that lie on the
        // same surface, so results don't accumulate needless topology.
        op.SimplifyResult();
        TopoDS_Shape result = collect_solids(op.Shape());
        if (result.IsNull()) {
          if (kind == BooleanKind::Common) return Solid{};
          return Diagnostic::error(
              "kernel.boolean.empty_result",
              fmt::format("The {} left nothing; the second solid covers the first", name));
        }
        return detail::finish_solid(std::move(result), "kernel.boolean.invalid_result",
                                    fmt::format("{}", name));
      });
}

}  // namespace

Result<Solid> unite(const Solid& a, const Solid& b, const BooleanOptions& options) {
  return run_boolean<BRepAlgoAPI_Fuse>(a, std::span<const Solid>(&b, 1), BooleanKind::Unite,
                                       options);
}
Result<Solid> cut(const Solid& a, const Solid& b, const BooleanOptions& options) {
  return run_boolean<BRepAlgoAPI_Cut>(a, std::span<const Solid>(&b, 1), BooleanKind::Cut,
                                      options);
}
Result<Solid> common(const Solid& a, const Solid& b, const BooleanOptions& options) {
  return run_boolean<BRepAlgoAPI_Common>(a, std::span<const Solid>(&b, 1), BooleanKind::Common,
                                         options);
}
Result<Shape> common_faces(const Face& a, const Face& b, const BooleanOptions& options) {
  if (a.is_empty() || b.is_empty()) {
    return Diagnostic::error("kernel.boolean.empty_input", "Common faces needs two faces");
  }
  if (!std::isfinite(options.fuzzy_mm) || options.fuzzy_mm < 0.0) {
    return Diagnostic::error("kernel.boolean.bad_fuzzy",
                             "The boolean fuzzy distance must be zero or positive");
  }
  return detail::guarded<Result<Shape>>(
      "kernel.boolean.failed", "The common of two faces", [&]() -> Result<Shape> {
        NCollection_List<TopoDS_Shape> arguments;
        NCollection_List<TopoDS_Shape> tools;
        arguments.Append(topo(a));
        tools.Append(topo(b));
        BRepAlgoAPI_Common op;
        op.SetArguments(arguments);
        op.SetTools(tools);
        // Never change the inputs in place (they are shared, immutable
        // handles): where the operation has to widen a tolerance or split
        // an edge of an argument, it works on a copy (plan H1).
        op.SetNonDestructive(true);
        if (options.fuzzy_mm > 0.0) op.SetFuzzyValue(options.fuzzy_mm);
        op.Build();
        if (op.HasErrors()) {
          std::ostringstream detail_text;
          op.DumpErrors(detail_text);
          return Diagnostic::error(
              "kernel.boolean.failed",
              fmt::format("The common of two faces failed in the kernel ({})", detail_text.str()));
        }
        std::vector<TopoDS_Shape> found;
        for (TopExp_Explorer ex(op.Shape(), TopAbs_FACE); ex.More(); ex.Next()) {
          found.push_back(ex.Current());
        }
        if (found.empty()) return Shape{};
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        for (const auto& f : found) builder.Add(compound, f);
        return detail::wrap<Shape>(compound);
      });
}

Result<Solid> unite(const Solid& a, std::span<const Solid> tools, const BooleanOptions& options) {
  if (tools.empty()) return a;
  return run_boolean<BRepAlgoAPI_Fuse>(a, tools, BooleanKind::Unite, options);
}
Result<Solid> cut(const Solid& a, std::span<const Solid> tools, const BooleanOptions& options) {
  if (tools.empty()) return a;
  return run_boolean<BRepAlgoAPI_Cut>(a, tools, BooleanKind::Cut, options);
}

Result<Shape> transformed(const Shape& shape, const core::dtransform& transform) {
  if (shape.is_empty()) return shape;
  return detail::guarded<Result<Shape>>(
      "kernel.transform.failed", "The transform", [&]() -> Result<Shape> {
        const auto& m = transform.linear;
        const auto& t = transform.translation;
        const double det = core::determinant(m);
        if (!std::isfinite(det) || std::abs(det) < 1e-12) {
          return Diagnostic::error("kernel.transform.singular",
                                   "The transform squashes space flat and can't be applied");
        }
        // A similarity (rotation, reflection and uniform scale) keeps the
        // geometry exact; check whether mᵀm is a multiple of the identity.
        const auto mtm = core::transpose(m) * m;
        const double s2 = mtm[0][0];
        bool similarity = true;
        for (std::size_t c = 0; c < 3 && similarity; ++c) {
          for (std::size_t r = 0; r < 3; ++r) {
            const double expected = r == c ? s2 : 0.0;
            if (std::abs(mtm[c][r] - expected) > 1e-12 * s2) {
              similarity = false;
              break;
            }
          }
        }
        // m[c][r] is column c, row r.
        if (similarity) {
          gp_Trsf trsf;
          trsf.SetValues(m[0][0], m[1][0], m[2][0], t.x,  //
                         m[0][1], m[1][1], m[2][1], t.y,  //
                         m[0][2], m[1][2], m[2][2], t.z);
          BRepBuilderAPI_Transform op(topo(shape), trsf, /*theCopyGeom=*/true);
          if (!op.IsDone()) {
            return Diagnostic::error("kernel.transform.failed", "The transform failed");
          }
          return detail::wrap(op.Shape());
        }
        gp_GTrsf gtrsf;
        gtrsf.SetVectorialPart(gp_Mat(m[0][0], m[1][0], m[2][0],  //
                                      m[0][1], m[1][1], m[2][1],  //
                                      m[0][2], m[1][2], m[2][2]));
        gtrsf.SetTranslationPart(gp_XYZ(t.x, t.y, t.z));
        BRepBuilderAPI_GTransform op(topo(shape), gtrsf, /*Copy=*/true);
        if (!op.IsDone()) {
          return Diagnostic::error("kernel.transform.failed", "The transform failed");
        }
        return detail::wrap(op.Shape());
      });
}

Result<Solid> transformed(const Solid& solid, const core::dtransform& transform) {
  auto shape = transformed(static_cast<const Shape&>(solid), transform);
  if (!shape) return shape.error();
  return detail::finish_solid(topo(*shape), "kernel.transform.invalid_result", "transform");
}

Result<Solid> deep_copy(const Solid& solid) {
  if (solid.is_empty()) return solid;
  return detail::guarded<Result<Solid>>("kernel.copy.failed", "Copying a solid",
                                        [&]() -> Result<Solid> {
                                          BRepBuilderAPI_Copy copy(topo(solid), true, false);
                                          return detail::wrap<Solid>(copy.Shape());
                                        });
}

Result<Solid> fillet(const Solid& solid, std::span<const Edge> edges, double radius) {
  if (!std::isfinite(radius) || radius <= 0.0) {
    return Diagnostic::error("kernel.fillet.invalid_radius",
                             "The fillet radius must be a positive number");
  }
  if (edges.empty()) return solid;
  const std::string advice = fmt::format(
      "Could not round {} edge(s) with a {} mm radius. The radius is probably larger than a "
      "neighbouring face allows: try a smaller radius, or fillet fewer edges at a time.",
      edges.size(), radius);
  auto result = detail::guarded<Result<Solid>>(
      "kernel.fillet.failed", "The fillet", [&]() -> Result<Solid> {
        NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> own;
        TopExp::MapShapes(topo(solid), TopAbs_EDGE, own);
        BRepFilletAPI_MakeFillet op(topo(solid));
        for (const Edge& e : edges) {
          if (!own.Contains(topo(e))) {
            return Diagnostic::error("kernel.fillet.foreign_edge",
                                     "An edge to fillet doesn't belong to the solid");
          }
          op.Add(radius, TopoDS::Edge(topo(e)));
        }
        op.Build();
        if (!op.IsDone()) return Diagnostic::error("kernel.fillet.failed", advice);
        return detail::finish_solid(op.Shape(), "kernel.fillet.failed", "fillet");
      });
  // Whatever went wrong inside the kernel, lead with what to try next.
  if (!result && result.error().code == "kernel.fillet.failed" &&
      result.error().message != advice) {
    return Diagnostic::error("kernel.fillet.failed",
                             fmt::format("{} ({})", advice, result.error().message));
  }
  return result;
}

Result<Solid> chamfer(const Solid& solid, std::span<const Edge> edges, double distance) {
  if (!std::isfinite(distance) || distance <= 0.0) {
    return Diagnostic::error("kernel.chamfer.invalid_distance",
                             "The chamfer distance must be a positive number");
  }
  if (edges.empty()) return solid;
  const std::string advice = fmt::format(
      "Could not chamfer {} edge(s) by {} mm. The distance is probably larger than a "
      "neighbouring face allows: try a smaller distance, or chamfer fewer edges at a time.",
      edges.size(), distance);
  auto result = detail::guarded<Result<Solid>>(
      "kernel.chamfer.failed", "The chamfer", [&]() -> Result<Solid> {
        NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> own;
        TopExp::MapShapes(topo(solid), TopAbs_EDGE, own);
        BRepFilletAPI_MakeChamfer op(topo(solid));
        for (const Edge& e : edges) {
          if (!own.Contains(topo(e))) {
            return Diagnostic::error("kernel.chamfer.foreign_edge",
                                     "An edge to chamfer doesn't belong to the solid");
          }
          op.Add(distance, TopoDS::Edge(topo(e)));
        }
        op.Build();
        if (!op.IsDone()) return Diagnostic::error("kernel.chamfer.failed", advice);
        return detail::finish_solid(op.Shape(), "kernel.chamfer.failed", "chamfer");
      });
  if (!result && result.error().code == "kernel.chamfer.failed" &&
      result.error().message != advice) {
    return Diagnostic::error("kernel.chamfer.failed",
                             fmt::format("{} ({})", advice, result.error().message));
  }
  return result;
}

Result<Solid> shell(const Solid& solid, std::span<const Face> open_faces, double thickness,
                    bool outward, const core::ProductSettings& settings) {
  if (auto d = detail::check_length("kernel.shell.invalid_thickness", "wall thickness",
                                    thickness, settings)) {
    return *d;
  }
  if (open_faces.empty()) {
    return Diagnostic::error("kernel.shell.no_faces",
                             "A shell needs at least one face to leave open");
  }
  const std::string advice = fmt::format(
      "Could not shell the body with {} mm walls. The walls are probably thicker than a curved "
      "face's radius or a narrow part of the body allows, or an open face meets a rounded "
      "edge: try thinner walls, open different faces, or shell before adding fillets.",
      thickness);
  auto result = detail::guarded<Result<Solid>>(
      "kernel.shell.failed", "The shell", [&]() -> Result<Solid> {
        NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> own;
        TopExp::MapShapes(topo(solid), TopAbs_FACE, own);
        NCollection_List<TopoDS_Shape> closing;
        NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> open;
        for (const Face& f : open_faces) {
          if (!own.Contains(topo(f))) {
            return Diagnostic::error("kernel.shell.foreign_face",
                                     "A face to open doesn't belong to the solid");
          }
          closing.Append(topo(f));
          open.Add(topo(f));
        }
        // OCCT's offset happily turns a curved wall inside out when the
        // walls are thicker than its radius, and the result can still pass
        // the validity check. Catch that case before it starts: a wall that
        // curves towards the side the shell grows to shrinks by `thickness`.
        for (int i = 1; i <= own.Extent(); ++i) {
          if (open.Contains(own(i))) continue;
          const TopoDS_Face& f = TopoDS::Face(own(i));
          BRepAdaptor_Surface surface(f, /*theRestriction=*/false);
          double radius = 0.0;
          bool direct = true;
          if (surface.GetType() == GeomAbs_Cylinder) {
            radius = surface.Cylinder().Radius();
            direct = surface.Cylinder().Direct();
          } else if (surface.GetType() == GeomAbs_Sphere) {
            radius = surface.Sphere().Radius();
            direct = surface.Sphere().Position().Direct();
          } else {
            continue;
          }
          // Convex: the face's normal points away from the centre.
          const bool convex = direct != (f.Orientation() == TopAbs_REVERSED);
          const bool shrinks = outward ? !convex : convex;
          if (shrinks && thickness >= radius - settings.linear_tolerance_mm) {
            return Diagnostic::error(
                "kernel.shell.too_thick",
                fmt::format("The walls ({} mm) are at least as thick as the radius of a curved "
                            "face ({} mm), which would turn that face inside out: use walls "
                            "thinner than {} mm{}",
                            thickness, radius, radius,
                            outward ? "" : ", or shell outwards"));
          }
        }
        BRepOffsetAPI_MakeThickSolid op;
        op.MakeThickSolidByJoin(topo(solid), closing, outward ? thickness : -thickness,
                                settings.linear_tolerance_mm, BRepOffset_Skin,
                                /*Intersection=*/false, /*SelfInter=*/false,
                                GeomAbs_Intersection);
        if (!op.IsDone()) return Diagnostic::error("kernel.shell.failed", advice);
        auto out = detail::finish_solid(op.Shape(), "kernel.shell.failed", "shell");
        if (!out) return out;
        if (auto w = detail::check_in_working_space(*out, settings)) return *w;
        return out;
      });
  if (!result && result.error().code == "kernel.shell.failed" &&
      result.error().message != advice) {
    return Diagnostic::error("kernel.shell.failed",
                             fmt::format("{} ({})", advice, result.error().message));
  }
  return result;
}

}  // namespace vs::kernel

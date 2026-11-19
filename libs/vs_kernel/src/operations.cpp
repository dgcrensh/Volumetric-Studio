#include "vs/kernel/operations.hpp"

#include <cmath>
#include <sstream>
#include <vector>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRep_Builder.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_GTrsf.hxx>
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
Result<Solid> run_boolean(const Solid& a, const Solid& b, BooleanKind kind,
                          const BooleanOptions& options) {
  const std::string_view name = name_of(kind);
  if (a.is_empty() || b.is_empty()) {
    return Diagnostic::error("kernel.boolean.empty_input",
                             fmt::format("The {} needs two non-empty solids", name));
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
        tools.Append(topo(b));
        Op op;
        op.SetArguments(arguments);
        op.SetTools(tools);
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
  return run_boolean<BRepAlgoAPI_Fuse>(a, b, BooleanKind::Unite, options);
}
Result<Solid> cut(const Solid& a, const Solid& b, const BooleanOptions& options) {
  return run_boolean<BRepAlgoAPI_Cut>(a, b, BooleanKind::Cut, options);
}
Result<Solid> common(const Solid& a, const Solid& b, const BooleanOptions& options) {
  return run_boolean<BRepAlgoAPI_Common>(a, b, BooleanKind::Common, options);
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

}  // namespace vs::kernel

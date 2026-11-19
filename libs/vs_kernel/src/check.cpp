#include "vs/kernel/check.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#include <BRepCheck.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Result.hxx>
#include <BRep_Tool.hxx>
#include <NCollection_IndexedMap.hxx>
#include <TopExp.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>

#include "occt.hpp"

namespace vs::kernel {

using detail::topo;

namespace {

using ShapeMap = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;

std::string status_name(BRepCheck_Status status) {
  std::ostringstream out;
  BRepCheck::Print(status, out);
  std::string s = out.str();
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

}  // namespace

Diagnostics check_validity(const Shape& shape) {
  Diagnostics out;
  if (shape.is_empty()) return out;
  try {
    const BRepCheck_Analyzer analyzer(topo(shape));
    if (analyzer.IsValid()) return out;

    // Name the failing checks, with how many sub-shapes fail each one.
    std::map<std::string, int> failures;
    constexpr TopAbs_ShapeEnum kTypes[] = {TopAbs_VERTEX, TopAbs_EDGE,  TopAbs_WIRE,
                                           TopAbs_FACE,   TopAbs_SHELL, TopAbs_SOLID};
    for (const TopAbs_ShapeEnum type : kTypes) {
      ShapeMap map;
      TopExp::MapShapes(topo(shape), type, map);
      for (int i = 1; i <= map.Extent(); ++i) {
        const auto& result = analyzer.Result(map(i));
        if (result.IsNull()) continue;
        for (const BRepCheck_Status status : result->Status()) {
          if (status != BRepCheck_NoError) ++failures[status_name(status)];
        }
      }
    }
    std::string detail_text;
    for (const auto& [name, count] : failures) {
      if (!detail_text.empty()) detail_text += ", ";
      detail_text += fmt::format("{} ({})", name, count);
    }
    if (detail_text.empty()) detail_text = "no detail available";
    out.add(Diagnostic::error("kernel.check.invalid",
                              fmt::format("The shape failed the kernel's validity check: {}",
                                          detail_text)));
  } catch (const std::exception& e) {
    out.add(Diagnostic::error("kernel.check.invalid",
                              fmt::format("The validity check itself failed: {}", e.what())));
  }
  return out;
}

double ToleranceAudit::max_tolerance() const {
  return std::max({vertices.max, edges.max, faces.max});
}

Diagnostics ToleranceAudit::diagnostics() const {
  Diagnostics out;
  const auto report = [&](const ToleranceStats& stats, std::string_view kind) {
    if (stats.exceeding == 0) return;
    out.add(Diagnostic::warning(
        "kernel.tolerance.exceeded",
        fmt::format("{} of {} {} have a kernel tolerance above the product tolerance "
                    "({:.3g} mm); the largest is {:.3g} mm",
                    stats.exceeding, stats.count, kind, limit_mm, stats.max)));
  };
  report(vertices, "vertices");
  report(edges, "edges");
  report(faces, "faces");
  return out;
}

ToleranceAudit audit_tolerances(const Shape& shape, const core::ProductSettings& settings) {
  ToleranceAudit audit;
  audit.limit_mm = settings.linear_tolerance_mm;
  if (shape.is_empty()) return audit;

  const auto measure = [&](TopAbs_ShapeEnum type, ToleranceStats& stats, auto tolerance_of) {
    ShapeMap map;
    TopExp::MapShapes(topo(shape), type, map);
    stats.count = static_cast<std::size_t>(map.Extent());
    for (int i = 1; i <= map.Extent(); ++i) {
      const double tol = tolerance_of(map(i));
      stats.max = std::max(stats.max, tol);
      if (tol > audit.limit_mm) ++stats.exceeding;
    }
  };
  measure(TopAbs_VERTEX, audit.vertices,
          [](const TopoDS_Shape& s) { return BRep_Tool::Tolerance(TopoDS::Vertex(s)); });
  measure(TopAbs_EDGE, audit.edges,
          [](const TopoDS_Shape& s) { return BRep_Tool::Tolerance(TopoDS::Edge(s)); });
  measure(TopAbs_FACE, audit.faces,
          [](const TopoDS_Shape& s) { return BRep_Tool::Tolerance(TopoDS::Face(s)); });
  return audit;
}

}  // namespace vs::kernel

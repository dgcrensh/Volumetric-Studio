#include <cmath>

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Iterator.hxx>

#include "occt.hpp"
#include "vs/kernel/check.hpp"
#include "vs/kernel/queries.hpp"

namespace vs::kernel {

namespace detail {

const TopoDS_Shape& ShapeAccess::get(const Shape& s) {
  static const TopoDS_Shape kNull;
  return s.data_ ? s.data_->shape : kNull;
}

bool is_finite(const core::dvec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

std::optional<Diagnostic> check_length(std::string_view code, std::string_view what, double value,
                                       const core::ProductSettings& settings) {
  // Sizes computed from coordinates (max - min) can land a rounding error
  // below the limit, so the comparison allows the linear tolerance.
  if (!std::isfinite(value) ||
      value < settings.min_feature_size_mm() - settings.linear_tolerance_mm) {
    return Diagnostic::error(
        std::string(code),
        fmt::format("The {} ({} mm) is smaller than the minimum feature size ({} mm)", what,
                    value, settings.min_feature_size_mm()));
  }
  return std::nullopt;
}

std::optional<Diagnostic> check_in_working_space(const Shape& shape,
                                                 const core::ProductSettings& settings) {
  const core::dbox3 box = bounding_box(shape);
  if (!settings.working_box().contains(box, settings.linear_tolerance_mm)) {
    return Diagnostic::error(
        "kernel.working_space.exceeded",
        fmt::format("The shape reaches outside the working space (±{} mm)",
                    settings.working_half_extent_mm));
  }
  return std::nullopt;
}

Result<Solid> finish_solid(TopoDS_Shape shape, std::string_view invalid_code,
                           std::string_view what) {
  auto solid = Solid::from(wrap(std::move(shape)));
  if (!solid) return solid;
  const Diagnostics problems = check_validity(*solid);
  if (problems.has_errors()) {
    return Diagnostic::error(std::string(invalid_code),
                             fmt::format("The {} produced an invalid solid. {}", what,
                                         problems[0].message));
  }
  return solid;
}

}  // namespace detail

using detail::topo;

std::string_view to_string(ShapeKind kind) {
  switch (kind) {
    case ShapeKind::Empty: return "empty";
    case ShapeKind::Compound: return "compound";
    case ShapeKind::CompSolid: return "compsolid";
    case ShapeKind::Solid: return "solid";
    case ShapeKind::Shell: return "shell";
    case ShapeKind::Face: return "face";
    case ShapeKind::Wire: return "wire";
    case ShapeKind::Edge: return "edge";
    case ShapeKind::Vertex: return "vertex";
  }
  return "unknown";
}

ShapeKind Shape::kind() const {
  const TopoDS_Shape& s = topo(*this);
  if (s.IsNull()) return ShapeKind::Empty;
  switch (s.ShapeType()) {
    case TopAbs_COMPOUND: return ShapeKind::Compound;
    case TopAbs_COMPSOLID: return ShapeKind::CompSolid;
    case TopAbs_SOLID: return ShapeKind::Solid;
    case TopAbs_SHELL: return ShapeKind::Shell;
    case TopAbs_FACE: return ShapeKind::Face;
    case TopAbs_WIRE: return ShapeKind::Wire;
    case TopAbs_EDGE: return ShapeKind::Edge;
    case TopAbs_VERTEX: return ShapeKind::Vertex;
    case TopAbs_SHAPE: break;
  }
  return ShapeKind::Empty;
}

bool Shape::is_same(const Shape& other) const {
  const TopoDS_Shape& a = topo(*this);
  const TopoDS_Shape& b = topo(other);
  if (a.IsNull() || b.IsNull()) return a.IsNull() && b.IsNull();
  return a.IsSame(b);
}

namespace {

Diagnostic wrong_kind(const Shape& shape, std::string_view wanted) {
  return Diagnostic::error("kernel.shape.wrong_kind",
                           fmt::format("Expected {}, got {}", wanted, to_string(shape.kind())));
}

// True when every leaf of a compound is a solid, and there is at least one.
bool only_solids(const TopoDS_Shape& s, std::size_t& count) {
  if (s.ShapeType() == TopAbs_SOLID) {
    ++count;
    return true;
  }
  if (s.ShapeType() != TopAbs_COMPOUND && s.ShapeType() != TopAbs_COMPSOLID) return false;
  for (TopoDS_Iterator it(s); it.More(); it.Next()) {
    if (!only_solids(it.Value(), count)) return false;
  }
  return true;
}

template <typename T>
Result<T> from_single(const Shape& shape, TopAbs_ShapeEnum type, std::string_view wanted) {
  const TopoDS_Shape& s = topo(shape);
  if (s.IsNull() || s.ShapeType() != type) return wrong_kind(shape, wanted);
  return detail::wrap<T>(s);
}

}  // namespace

Result<Solid> Solid::from(const Shape& shape) {
  const TopoDS_Shape& s = topo(shape);
  std::size_t count = 0;
  if (s.IsNull() || !only_solids(s, count) || count == 0) {
    return wrong_kind(shape, "a solid or a compound of solids");
  }
  return detail::wrap<Solid>(s);
}

std::size_t Solid::solid_count() const {
  std::size_t count = 0;
  for (TopExp_Explorer ex(topo(*this), TopAbs_SOLID); ex.More(); ex.Next()) ++count;
  return count;
}

Result<Shell> Shell::from(const Shape& shape) {
  return from_single<Shell>(shape, TopAbs_SHELL, "a shell");
}
Result<Face> Face::from(const Shape& shape) {
  return from_single<Face>(shape, TopAbs_FACE, "a face");
}
Result<Edge> Edge::from(const Shape& shape) {
  return from_single<Edge>(shape, TopAbs_EDGE, "an edge");
}

}  // namespace vs::kernel

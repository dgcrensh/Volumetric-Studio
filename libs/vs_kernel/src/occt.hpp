#pragma once

// Private glue between the public kernel handles and Open CASCADE. Only
// vs_kernel's sources include this header.

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include "vs/core/settings.hpp"
#include "vs/kernel/shape.hpp"

namespace vs::kernel::detail {

struct ShapeData {
  TopoDS_Shape shape;
};

// The one place that reaches into a handle's private data.
struct ShapeAccess {
  static const TopoDS_Shape& get(const Shape& s);

  template <typename T>
  static T wrap(TopoDS_Shape shape) {
    T out;
    if (!shape.IsNull()) {
      static_cast<Shape&>(out).data_ = std::make_shared<const ShapeData>(ShapeData{std::move(shape)});
    }
    return out;
  }
};

inline const TopoDS_Shape& topo(const Shape& s) { return ShapeAccess::get(s); }

template <typename T = Shape>
T wrap(TopoDS_Shape shape) {
  return ShapeAccess::wrap<T>(std::move(shape));
}

inline gp_Pnt to_occ(const core::dvec3& p) { return {p.x, p.y, p.z}; }
inline gp_Vec to_occ_vec(const core::dvec3& v) { return {v.x, v.y, v.z}; }
inline core::dvec3 from_occ(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }
inline core::dvec3 from_occ(const gp_Vec& v) { return {v.X(), v.Y(), v.Z()}; }
inline core::dvec3 from_occ(const gp_Dir& d) { return {d.X(), d.Y(), d.Z()}; }

// Runs `body`, turning any exception (OCCT's Standard_Failure derives from
// std::exception) into an error diagnostic with `code`. `body` returns a
// Result<T> or anything convertible to one.
template <typename R, typename F>
R guarded(std::string_view code, std::string_view what, F&& body) {
  try {
    return body();
  } catch (const std::exception& e) {
    return Diagnostic::error(std::string(code),
                             fmt::format("{} failed inside the kernel: {}", what, e.what()));
  } catch (...) {
    return Diagnostic::error(std::string(code),
                             fmt::format("{} failed inside the kernel", what));
  }
}

// Shared input checks. Each returns an error diagnostic or nothing.
bool is_finite(const core::dvec3& v);
std::optional<Diagnostic> check_length(std::string_view code, std::string_view what, double value,
                                       const core::ProductSettings& settings);
std::optional<Diagnostic> check_in_working_space(const Shape& shape,
                                                 const core::ProductSettings& settings);

// A solid result from a TopoDS shape: checks the kind and validity.
Result<Solid> finish_solid(TopoDS_Shape shape, std::string_view invalid_code,
                           std::string_view what);

}  // namespace vs::kernel::detail

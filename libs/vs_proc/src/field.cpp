#include "vs/proc/field.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <utility>

#include <fmt/format.h>

namespace vs::proc {

using core::Diagnostic;

namespace {

dvec3 vec_at(const std::vector<double>& n, std::size_t i) { return {n[i], n[i + 1], n[i + 2]}; }

Field make(FieldOp op, std::vector<double> numbers, std::vector<Field> children = {},
           std::shared_ptr<const TriangleDistance> mesh = nullptr) {
  return std::make_shared<const FieldExpr>(op, std::move(numbers), std::move(children),
                                           std::move(mesh));
}

double box_sdf(const dvec3& p, const dvec3& centre, const dvec3& half) {
  const dvec3 q{std::abs(p.x - centre.x) - half.x, std::abs(p.y - centre.y) - half.y,
                std::abs(p.z - centre.z) - half.z};
  const dvec3 outside{std::max(q.x, 0.0), std::max(q.y, 0.0), std::max(q.z, 0.0)};
  return length(outside) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0);
}

}  // namespace

FieldExpr::FieldExpr(FieldOp op, std::vector<double> numbers, std::vector<Field> children,
                     std::shared_ptr<const TriangleDistance> mesh)
    : op_(op), numbers_(std::move(numbers)), children_(std::move(children)), mesh_(std::move(mesh)) {
  Hasher h;
  h.add(std::string_view("vstudio.proc.field"));
  h.add(op_);
  h.add(static_cast<std::uint64_t>(numbers_.size()));
  for (const double x : numbers_) h.add(x);
  h.add(static_cast<std::uint64_t>(children_.size()));
  for (const auto& c : children_) h.add(c->hash());
  h.add(mesh_ != nullptr);
  if (mesh_) h.add(mesh_->hash());
  hash_ = h.finalize();
}

double smooth_min(double a, double b, double radius) {
  if (!(radius > 0.0)) return std::min(a, b);
  const double h = std::clamp(0.5 + 0.5 * (b - a) / radius, 0.0, 1.0);
  return b + (a - b) * h - radius * h * (1.0 - h);
}

double smooth_max(double a, double b, double radius) { return -smooth_min(-a, -b, radius); }

double FieldExpr::eval(const dvec3& p) const {
  const auto& n = numbers_;
  switch (op_) {
    case FieldOp::Constant:
      return n[0];
    case FieldOp::Sphere:
      return length(p - vec_at(n, 0)) - n[3];
    case FieldOp::Box:
      return box_sdf(p, vec_at(n, 0), vec_at(n, 3));
    case FieldOp::Cylinder: {
      const dvec3 axis = vec_at(n, 3);
      const dvec3 d = p - vec_at(n, 0);
      const double along = dot(d, axis);
      const double radial = length(d - axis * along);
      const double qx = radial - n[6];
      const double qy = std::abs(along) - n[7];
      const double ox = std::max(qx, 0.0);
      const double oy = std::max(qy, 0.0);
      return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0);
    }
    case FieldOp::Plane:
      return std::abs(dot(p - vec_at(n, 0), vec_at(n, 3)));
    case FieldOp::HalfSpace:
      return dot(p - vec_at(n, 0), vec_at(n, 3));
    case FieldOp::MeshSigned:
      return mesh_->signed_distance(p);
    case FieldOp::MeshUnsigned:
      return mesh_->distance(p);
    case FieldOp::Union:
      return std::min(children_[0]->eval(p), children_[1]->eval(p));
    case FieldOp::Intersection:
      return std::max(children_[0]->eval(p), children_[1]->eval(p));
    case FieldOp::Difference:
      return std::max(children_[0]->eval(p), -children_[1]->eval(p));
    case FieldOp::SmoothUnion:
      return smooth_min(children_[0]->eval(p), children_[1]->eval(p), n[0]);
    case FieldOp::SmoothIntersection:
      return smooth_max(children_[0]->eval(p), children_[1]->eval(p), n[0]);
    case FieldOp::SmoothDifference:
      return smooth_max(children_[0]->eval(p), -children_[1]->eval(p), n[0]);
    case FieldOp::Offset:
      return children_[0]->eval(p) - n[0];
    case FieldOp::Shell: {
      const double d = children_[0]->eval(p);
      return std::max(d, -d - n[0]);
    }
    case FieldOp::Negate:
      return -children_[0]->eval(p);
    case FieldOp::Abs:
      return std::abs(children_[0]->eval(p));
    case FieldOp::Add:
      return children_[0]->eval(p) + children_[1]->eval(p);
    case FieldOp::Multiply:
      return children_[0]->eval(p) * children_[1]->eval(p);
    case FieldOp::Scale:
      return children_[0]->eval(p) * n[0];
  }
  return std::numeric_limits<double>::quiet_NaN();
}

Field constant_field(double value) { return make(FieldOp::Constant, {value}); }

Field sphere_field(const dvec3& c, double radius) {
  return make(FieldOp::Sphere, {c.x, c.y, c.z, radius});
}

Field box_field(const dvec3& c, const dvec3& size) {
  return make(FieldOp::Box, {c.x, c.y, c.z, size.x * 0.5, size.y * 0.5, size.z * 0.5});
}

Field cylinder_field(const dvec3& c, const dvec3& axis, double radius, double height) {
  const dvec3 a = normalize(axis);
  return make(FieldOp::Cylinder, {c.x, c.y, c.z, a.x, a.y, a.z, radius, height * 0.5});
}

Field plane_field(const dvec3& p, const dvec3& normal) {
  const dvec3 nn = normalize(normal);
  return make(FieldOp::Plane, {p.x, p.y, p.z, nn.x, nn.y, nn.z});
}

Field half_space_field(const dvec3& p, const dvec3& normal) {
  const dvec3 nn = normalize(normal);
  return make(FieldOp::HalfSpace, {p.x, p.y, p.z, nn.x, nn.y, nn.z});
}

Field mesh_signed_field(std::shared_ptr<const TriangleDistance> mesh) {
  return make(FieldOp::MeshSigned, {}, {}, std::move(mesh));
}

Field mesh_unsigned_field(std::shared_ptr<const TriangleDistance> mesh) {
  return make(FieldOp::MeshUnsigned, {}, {}, std::move(mesh));
}

Field union_field(Field a, Field b) { return make(FieldOp::Union, {}, {std::move(a), std::move(b)}); }
Field intersection_field(Field a, Field b) {
  return make(FieldOp::Intersection, {}, {std::move(a), std::move(b)});
}
Field difference_field(Field a, Field b) {
  return make(FieldOp::Difference, {}, {std::move(a), std::move(b)});
}
Field smooth_union_field(Field a, Field b, double radius) {
  return make(FieldOp::SmoothUnion, {radius}, {std::move(a), std::move(b)});
}
Field smooth_intersection_field(Field a, Field b, double radius) {
  return make(FieldOp::SmoothIntersection, {radius}, {std::move(a), std::move(b)});
}
Field smooth_difference_field(Field a, Field b, double radius) {
  return make(FieldOp::SmoothDifference, {radius}, {std::move(a), std::move(b)});
}
Field offset_field(Field a, double distance) {
  return make(FieldOp::Offset, {distance}, {std::move(a)});
}
Field shell_field(Field a, double thickness) {
  return make(FieldOp::Shell, {thickness}, {std::move(a)});
}
Field negate_field(Field a) { return make(FieldOp::Negate, {}, {std::move(a)}); }
Field abs_field(Field a) { return make(FieldOp::Abs, {}, {std::move(a)}); }
Field add_field(Field a, Field b) { return make(FieldOp::Add, {}, {std::move(a), std::move(b)}); }
Field multiply_field(Field a, Field b) {
  return make(FieldOp::Multiply, {}, {std::move(a), std::move(b)});
}
Field scale_field(Field a, double factor) { return make(FieldOp::Scale, {factor}, {std::move(a)}); }

// --- sampling ------------------------------------------------------------------------

dvec3 GridSpec::point(std::uint32_t i, std::uint32_t j, std::uint32_t k) const {
  return {origin.x + static_cast<double>(i) * cell, origin.y + static_cast<double>(j) * cell,
          origin.z + static_cast<double>(k) * cell};
}

void hash_append(Hasher& h, const GridSpec& g) {
  h.add(g.origin).add(g.cell).add(g.nx).add(g.ny).add(g.nz);
}

core::Result<GridSpec> grid_covering(const dbox3& box, double cell) {
  if (box.is_empty()) {
    return Diagnostic::error("proc.sample.empty", "There's no box to sample: it's empty");
  }
  const dvec3 size = box.size();
  if (cell == 0.0) cell = std::max(size.x, std::max(size.y, size.z)) / 64.0;
  if (!(cell > 0.0) || !std::isfinite(cell)) {
    return Diagnostic::error("proc.sample.empty",
                             fmt::format("A grid cell of {} mm can't sample anything", cell));
  }
  const auto count = [&](double extent) {
    return std::ceil(extent / cell) + 1.0;
  };
  const double nx = count(size.x);
  const double ny = count(size.y);
  const double nz = count(size.z);
  if (nx * ny * nz > static_cast<double>(kMaxGridSamples)) {
    return Diagnostic::error(
        "proc.sample.too_large",
        fmt::format("A {} mm cell over this box needs {:.0f} samples, more than the {} allowed; "
                    "use a larger cell",
                    cell, nx * ny * nz, kMaxGridSamples));
  }
  GridSpec g;
  g.origin = box.min;
  g.cell = cell;
  g.nx = static_cast<std::uint32_t>(nx);
  g.ny = static_cast<std::uint32_t>(ny);
  g.nz = static_cast<std::uint32_t>(nz);
  return g;
}

Hash256 SampledField::hash() const {
  Hasher h;
  h.add(std::string_view("vstudio.proc.samples"));
  h.add(grid);
  h.add(static_cast<std::uint64_t>(values.size()));
  for (const float v : values) h.add(v);
  return h.finalize();
}

SampledField sample(const FieldExpr& field, const GridSpec& grid, eval::ThreadPool* pool,
                    const eval::CancellationToken* cancel, bool* cancelled) {
  SampledField out;
  out.grid = grid;
  out.values.assign(grid.size(), 0.0f);
  std::atomic<bool> stopped{false};
  const auto slice = [&](std::size_t k) {
    if (cancel != nullptr && cancel->cancelled()) {
      stopped = true;
      return;
    }
    const auto kk = static_cast<std::uint32_t>(k);
    for (std::uint32_t j = 0; j < grid.ny; ++j) {
      for (std::uint32_t i = 0; i < grid.nx; ++i) {
        out.values[grid.index(i, j, kk)] = static_cast<float>(field.eval(grid.point(i, j, kk)));
      }
    }
  };
  if (pool != nullptr) {
    pool->run(grid.nz, slice);
  } else {
    for (std::size_t k = 0; k < grid.nz; ++k) slice(k);
  }
  if (cancelled != nullptr) *cancelled = stopped.load();
  return out;
}

}  // namespace vs::proc

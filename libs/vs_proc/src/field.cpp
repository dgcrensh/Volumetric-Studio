#include "vs/proc/field.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

#include <fmt/format.h>

#include "vs/core/transform.hpp"

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

// Distance from p to the segment ab.
double segment_distance(const dvec3& p, const dvec3& a, const dvec3& b) {
  const dvec3 ab = b - a;
  const dvec3 ap = p - a;
  const double t = std::clamp(dot(ap, ab) / dot(ab, ab), 0.0, 1.0);
  return length(ap - ab * t);
}

struct Strut {
  dvec3 a, b;
};

std::vector<Strut> unit_struts(LatticeCell kind) {
  std::vector<Strut> out;
  const auto add = [&](dvec3 a, dvec3 b) { out.push_back({a, b}); };
  switch (kind) {
    case LatticeCell::Cubic:
      for (int u = 0; u <= 1; ++u) {
        for (int v = 0; v <= 1; ++v) {
          const double du = static_cast<double>(u), dv = static_cast<double>(v);
          add({0, du, dv}, {1, du, dv});
          add({du, 0, dv}, {du, 1, dv});
          add({du, dv, 0}, {du, dv, 1});
        }
      }
      break;
    case LatticeCell::Bcc:
      add({0, 0, 0}, {1, 1, 1});
      add({1, 0, 0}, {0, 1, 1});
      add({0, 1, 0}, {1, 0, 1});
      add({0, 0, 1}, {1, 1, 0});
      break;
    case LatticeCell::Octet: {
      // Face diagonals: two on each face.
      for (int axis = 0; axis < 3; ++axis) {
        for (int side = 0; side <= 1; ++side) {
          const auto pt = [&](double a, double b) {
            dvec3 q{};
            const double c = static_cast<double>(side);
            if (axis == 0) q = {c, a, b};
            if (axis == 1) q = {a, c, b};
            if (axis == 2) q = {a, b, c};
            return q;
          };
          add(pt(0, 0), pt(1, 1));
          add(pt(1, 0), pt(0, 1));
        }
      }
      // The octahedron: each face centre to the four centres beside it.
      const dvec3 centres[6] = {{0, 0.5, 0.5}, {1, 0.5, 0.5}, {0.5, 0, 0.5},
                                {0.5, 1, 0.5}, {0.5, 0.5, 0}, {0.5, 0.5, 1}};
      for (int i = 0; i < 6; ++i) {
        for (int j = i + 1; j < 6; ++j) {
          if (i / 2 == j / 2) continue;  // opposite faces
          add(centres[i], centres[j]);
        }
      }
      break;
    }
  }
  return out;
}

const std::vector<Strut>& struts(LatticeCell kind) {
  static const std::vector<Strut> cubic = unit_struts(LatticeCell::Cubic);
  static const std::vector<Strut> bcc = unit_struts(LatticeCell::Bcc);
  static const std::vector<Strut> octet = unit_struts(LatticeCell::Octet);
  switch (kind) {
    case LatticeCell::Cubic: return cubic;
    case LatticeCell::Bcc: return bcc;
    case LatticeCell::Octet: return octet;
  }
  return cubic;
}

// The point's position in its cell, in cell units ([0, 1) on each axis).
dvec3 in_cell(const dvec3& p, const dvec3& origin, double cell) {
  const dvec3 u = (p - origin) * (1.0 / cell);
  return {u.x - std::floor(u.x), u.y - std::floor(u.y), u.z - std::floor(u.z)};
}

double lattice_distance(LatticeCell kind, const dvec3& p, const dvec3& origin, double cell) {
  const dvec3 q = in_cell(p, origin, cell);
  double best = std::numeric_limits<double>::infinity();
  for (const Strut& s : struts(kind)) best = std::min(best, segment_distance(q, s.a, s.b));
  return best * cell;
}

// |f| / |grad f| for the surface at p, in mm.
double tpms_distance(TpmsSurface kind, const dvec3& p, const dvec3& origin, double cell) {
  const double w = 2.0 * std::numbers::pi / cell;
  const dvec3 x = (p - origin) * w;
  const double sx = sin_det(x.x), cx = cos_det(x.x);
  const double sy = sin_det(x.y), cy = cos_det(x.y);
  const double sz = sin_det(x.z), cz = cos_det(x.z);
  double f = 0.0;
  dvec3 g{};
  switch (kind) {
    case TpmsSurface::Gyroid:
      f = sx * cy + sy * cz + sz * cx;
      g = {cx * cy - sz * sx, cy * cz - sx * sy, cz * cx - sy * sz};
      break;
    case TpmsSurface::SchwarzP:
      f = cx + cy + cz;
      g = {-sx, -sy, -sz};
      break;
    case TpmsSurface::Diamond:
      f = sx * sy * sz + sx * cy * cz + cx * sy * cz + cx * cy * sz;
      g = {cx * sy * sz + cx * cy * cz - sx * sy * cz - sx * cy * sz,
           sx * cy * sz - sx * sy * cz + cx * cy * cz - cx * sy * sz,
           sx * sy * cz - sx * cy * sz - cx * sy * sz + cx * cy * cz};
      break;
  }
  // Away from the surface the gradient can vanish (at the surface's
  // critical points); a floor keeps the estimate finite there.
  const double grad = std::max(length(g), 0.1);
  return std::abs(f) / (w * grad);
}

}  // namespace

// --- deterministic trigonometry ------------------------------------------------------

namespace {

// pi/2 in three parts (fdlibm): k * part1 is exact for |k| < 2^20.
constexpr double kPio2Part1 = 1.57079632673412561417e+00;
constexpr double kPio2Part2 = 6.07710050630396597660e-11;
constexpr double kPio2Part3 = 2.02226624871116645580e-21;
constexpr double kTwoOverPi = 6.36619772367581382433e-01;

double kernel_sin(double x) {
  constexpr double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                   S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                   S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
  const double z = x * x;
  return x + x * z * (S1 + z * (S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)))));
}

double kernel_cos(double x) {
  constexpr double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                   C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                   C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
  const double z = x * x;
  return 1.0 - 0.5 * z + z * z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
}

// x = k pi/2 + r, |r| <= pi/4; returns r and k mod 4.
double reduce(double x, int& quadrant) {
  const double k = std::floor(x * kTwoOverPi + 0.5);
  const double r = ((x - k * kPio2Part1) - k * kPio2Part2) - k * kPio2Part3;
  const double m = k - 4.0 * std::floor(k * 0.25);
  quadrant = static_cast<int>(m);
  return r;
}

}  // namespace

double sin_det(double x) {
  if (!std::isfinite(x)) return std::numeric_limits<double>::quiet_NaN();
  int q = 0;
  const double r = reduce(x, q);
  switch (q) {
    case 0: return kernel_sin(r);
    case 1: return kernel_cos(r);
    case 2: return -kernel_sin(r);
    default: return -kernel_cos(r);
  }
}

double cos_det(double x) {
  if (!std::isfinite(x)) return std::numeric_limits<double>::quiet_NaN();
  int q = 0;
  const double r = reduce(x, q);
  switch (q) {
    case 0: return kernel_cos(r);
    case 1: return -kernel_sin(r);
    case 2: return -kernel_cos(r);
    default: return kernel_sin(r);
  }
}

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
    case FieldOp::Clamp:
      return std::clamp(children_[0]->eval(p), n[0], n[1]);
    case FieldOp::Lattice:
    case FieldOp::Tpms: {
      const dvec3 origin = vec_at(n, 2);
      const double thickness =
          children_.empty() ? n[5] : std::max(children_[0]->eval(p), 0.0);
      const double d = op_ == FieldOp::Lattice
                           ? lattice_distance(static_cast<LatticeCell>(static_cast<int>(n[0])), p, origin, n[1])
                           : tpms_distance(static_cast<TpmsSurface>(static_cast<int>(n[0])), p, origin, n[1]);
      return d - 0.5 * thickness;
    }
    case FieldOp::Instances: {
      // numbers: radius, then per copy the inverse's 3x3 (row major), the
      // frame's origin and the copy's reach (radius times its largest scale).
      double best = std::numeric_limits<double>::infinity();
      const std::size_t copies = (n.size() - 1) / 13;
      for (std::size_t c = 0; c < copies; ++c) {
        const double* m = n.data() + 1 + 13 * c;
        const dvec3 d{p.x - m[9], p.y - m[10], p.z - m[11]};
        if (m[12] > 0.0 && length(d) - m[12] >= best) continue;
        const dvec3 local{m[0] * d.x + m[1] * d.y + m[2] * d.z,
                          m[3] * d.x + m[4] * d.y + m[5] * d.z,
                          m[6] * d.x + m[7] * d.y + m[8] * d.z};
        best = std::min(best, children_[0]->eval(local));
      }
      return best;
    }
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
Field clamp_field(Field a, double lo, double hi) {
  return make(FieldOp::Clamp, {lo, hi}, {std::move(a)});
}

Field lattice_field(LatticeCell kind, double cell, const dvec3& o, double thickness, Field grade) {
  std::vector<Field> children;
  if (grade) children.push_back(std::move(grade));
  return make(FieldOp::Lattice,
              {static_cast<double>(kind), cell, o.x, o.y, o.z, grade ? 0.0 : thickness},
              std::move(children));
}

Field tpms_field(TpmsSurface kind, double cell, const dvec3& o, double thickness, Field grade) {
  std::vector<Field> children;
  if (grade) children.push_back(std::move(grade));
  return make(FieldOp::Tpms,
              {static_cast<double>(kind), cell, o.x, o.y, o.z, grade ? 0.0 : thickness},
              std::move(children));
}

Field instances_field(Field templ, const FrameSet& frames, double radius) {
  std::vector<double> n{radius};
  n.reserve(1 + 13 * frames.frames.size());
  for (const auto& f : frames.frames) {
    const core::dtransform t = core::to_fp64(f);
    const auto inv = core::inverse(t);
    if (!inv) continue;  // a degenerate frame places nothing
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) n.push_back(inv->linear(r, c));
    }
    n.push_back(t.translation.x);
    n.push_back(t.translation.y);
    n.push_back(t.translation.z);
    double scale = 0.0;
    for (const auto& col : t.linear.cols) scale = std::max(scale, length(col));
    n.push_back(radius * scale);
  }
  return make(FieldOp::Instances, std::move(n), {std::move(templ)});
}

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

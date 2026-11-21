#include "vs/mesh/seam.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <cmath>
#include <limits>
#include <optional>
#include <numbers>
#include <string>
#include <unordered_map>

namespace vs::mesh {
namespace {

using core::Diagnostic;
using core::Result;

constexpr double kPi = std::numbers::pi;
// Arcs get at least this many pieces per full turn.
constexpr double kArcPiecesPerTurn = 16.0;

std::size_t pieces(const OutlineSegment& s, double spacing) {
  const double by_length = std::ceil(s.length() / spacing);
  double n = std::max(1.0, by_length);
  if (s.kind == OutlineSegment::Kind::Arc) {
    n = std::max(n, std::ceil(std::abs(s.sweep) / (2.0 * kPi) * kArcPiecesPerTurn));
  }
  return static_cast<std::size_t>(n);
}

// The loop's points at a coarse division, for its orientation and extent.
std::vector<dvec2> coarse_points(const OutlineLoop& loop) {
  std::vector<dvec2> pts;
  for (const OutlineSegment& s : loop.segments) {
    const std::size_t n = s.kind == OutlineSegment::Kind::Arc ? 16 : 1;
    for (std::size_t k = 0; k < n; ++k) {
      pts.push_back(s.point(static_cast<double>(k) / static_cast<double>(n)));
    }
  }
  return pts;
}

double coarse_area(const OutlineLoop& loop) {
  const std::vector<dvec2> pts = coarse_points(loop);
  double a = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const dvec2& p = pts[i];
    const dvec2& q = pts[(i + 1) % pts.size()];
    a += p.x * q.y - q.x * p.y;
  }
  return a;
}

OutlineLoop reversed(const OutlineLoop& loop) {
  OutlineLoop out;
  for (auto it = loop.segments.rbegin(); it != loop.segments.rend(); ++it) {
    out.segments.push_back(it->reversed());
  }
  return out;
}

double point_segment_distance_sq(const dvec2& p, const dvec2& a, const dvec2& b) {
  const dvec2 ab = b - a;
  const double len_sq = core::dot(ab, ab);
  double t = len_sq > 0 ? core::dot(p - a, ab) / len_sq : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  const dvec2 d = p - (a + ab * t);
  return core::dot(d, d);
}

// Lattice points inside the loops, at least `clearance` from every segment.
std::vector<dvec2> interior_points(const std::vector<dvec2>& pts, const std::vector<Loop>& loops,
                                   double h, double clearance) {
  struct Seg {
    dvec2 a, b;
  };
  std::vector<Seg> segs;
  dvec2 lo = pts[loops[0][0]], hi = lo;
  for (const Loop& loop : loops) {
    for (std::size_t i = 0; i < loop.size(); ++i) {
      const dvec2& a = pts[loop[i]];
      const dvec2& b = pts[loop[(i + 1) % loop.size()]];
      segs.push_back({a, b});
      lo = core::min(lo, a);
      hi = core::max(hi, a);
    }
  }
  // Segments by grid cell (cell size h), padded by the clearance.
  const auto cell = [h](double x) { return static_cast<std::int64_t>(std::floor(x / h)); };
  const auto key = [](std::int64_t i, std::int64_t j) {
    return (static_cast<std::uint64_t>(i) << 32) ^ static_cast<std::uint64_t>(j & 0xffffffff);
  };
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> grid;
  for (std::uint32_t s = 0; s < segs.size(); ++s) {
    const dvec2 slo = core::min(segs[s].a, segs[s].b);
    const dvec2 shi = core::max(segs[s].a, segs[s].b);
    for (std::int64_t i = cell(slo.x - clearance); i <= cell(shi.x + clearance); ++i) {
      for (std::int64_t j = cell(slo.y - clearance); j <= cell(shi.y + clearance); ++j) {
        grid[key(i, j)].push_back(s);
      }
    }
  }
  const double row_step = h * std::sqrt(3.0) / 2.0;
  const double clearance_sq = clearance * clearance;
  std::vector<dvec2> out;
  std::vector<double> xs;
  const auto j0 = static_cast<std::int64_t>(std::ceil(lo.y / row_step));
  const auto j1 = static_cast<std::int64_t>(std::floor(hi.y / row_step));
  for (std::int64_t j = j0; j <= j1; ++j) {
    const double y = static_cast<double>(j) * row_step;
    const double offset = (j % 2 != 0) ? 0.5 * h : 0.0;
    xs.clear();
    for (const Seg& s : segs) {
      if ((s.a.y > y) != (s.b.y > y)) {
        xs.push_back(s.a.x + (y - s.a.y) * (s.b.x - s.a.x) / (s.b.y - s.a.y));
      }
    }
    std::sort(xs.begin(), xs.end());
    for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
      const auto i0 = static_cast<std::int64_t>(std::ceil((xs[k] - offset) / h));
      const auto i1 = static_cast<std::int64_t>(std::floor((xs[k + 1] - offset) / h));
      for (std::int64_t i = i0; i <= i1; ++i) {
        const dvec2 p{static_cast<double>(i) * h + offset, y};
        bool clear = true;
        if (const auto it = grid.find(key(cell(p.x), cell(p.y))); it != grid.end()) {
          for (const std::uint32_t s : it->second) {
            if (point_segment_distance_sq(p, segs[s].a, segs[s].b) < clearance_sq) {
              clear = false;
              break;
            }
          }
        }
        if (clear) out.push_back(p);
      }
    }
  }
  return out;
}

Diagnostic with_subject(Diagnostic d, const core::SubjectRef& subject) {
  d.subjects.insert(d.subjects.begin(), subject);
  return d;
}

// --- cylinders (L2) ---------------------------------------------------------------

// The unrolled rectangle a cylinder outline spans.
struct Span {
  double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
  [[nodiscard]] double width() const { return x1 - x0; }
  [[nodiscard]] double height() const { return y1 - y0; }
};

std::optional<Span> cylinder_span(const SeamOutline& outline) {
  Span s{std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
         std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
  for (const OutlineLoop& loop : outline.loops) {
    for (const OutlineSegment& seg : loop.segments) {
      if (seg.kind != OutlineSegment::Kind::Line) return std::nullopt;
      // Lines run along x or along y only.
      if (seg.start.x != seg.end.x && seg.start.y != seg.end.y) return std::nullopt;
      for (const dvec2& p : {seg.start, seg.end}) {
        s.x0 = std::min(s.x0, p.x);
        s.x1 = std::max(s.x1, p.x);
        s.y0 = std::min(s.y0, p.y);
        s.y1 = std::max(s.y1, p.y);
      }
    }
  }
  const std::size_t want = outline.band ? 2 : 1;
  if (outline.loops.size() != want || !(s.width() > 0) || !(s.height() > 0)) return std::nullopt;
  if (outline.band && std::abs(s.width() - 2.0 * kPi * outline.radius) >
                          1e-9 * (1.0 + 2.0 * kPi * outline.radius)) {
    return std::nullopt;
  }
  return s;
}

// Pieces along an unrolled line: by length, and lines round the axis at
// least as finely as arcs are (kArcPiecesPerTurn per turn).
std::size_t cylinder_pieces(const OutlineSegment& s, double spacing, double radius) {
  double n = std::max(1.0, std::ceil(s.length() / spacing));
  if (s.start.y == s.end.y) {
    n = std::max(n, std::ceil(std::abs(s.end.x - s.start.x) / radius / (2.0 * kPi) *
                              kArcPiecesPerTurn));
  }
  return static_cast<std::size_t>(n);
}

dvec3 cylinder_point(const sketch::SketchPlane& f, double radius, const dvec2& uv) {
  const double a = uv.x / radius;
  return f.origin + f.x_axis * (radius * std::cos(a)) + f.y_axis * (radius * std::sin(a)) +
         f.normal() * uv.y;
}

}  // namespace

// --- OutlineSegment -------------------------------------------------------------

OutlineSegment OutlineSegment::line(const dvec2& a, const dvec2& b) {
  OutlineSegment s;
  s.kind = Kind::Line;
  s.start = a;
  s.end = b;
  return s;
}

OutlineSegment OutlineSegment::arc(const dvec2& centre, double radius, double start_angle,
                                   double sweep) {
  OutlineSegment s;
  s.kind = Kind::Arc;
  s.centre = centre;
  s.radius = radius;
  s.start_angle = start_angle;
  s.sweep = sweep;
  s.start = s.point(0.0);
  s.end = s.point(1.0);
  return s;
}

OutlineSegment OutlineSegment::circle(const dvec2& centre, double radius) {
  return arc(centre, radius, 0.0, 2.0 * kPi);
}

double OutlineSegment::length() const {
  if (kind == Kind::Line) return core::length(end - start);
  return std::abs(sweep) * radius;
}

dvec2 OutlineSegment::point(double t) const {
  if (kind == Kind::Line) {
    if (t == 0.0) return start;
    if (t == 1.0) return end;
    return start + (end - start) * t;
  }
  const double a = start_angle + sweep * t;
  return {centre.x + radius * std::cos(a), centre.y + radius * std::sin(a)};
}

OutlineSegment OutlineSegment::reversed() const {
  if (kind == Kind::Line) return line(end, start);
  OutlineSegment s = *this;
  s.start_angle = start_angle + sweep;
  s.sweep = -sweep;
  std::swap(s.start, s.end);
  return s;
}

void hash_append(core::Hasher& h, const OutlineSegment& s) {
  h.add(s.kind);
  h.add(s.start.x).add(s.start.y).add(s.end.x).add(s.end.y);
  h.add(s.centre.x).add(s.centre.y).add(s.radius).add(s.start_angle).add(s.sweep);
}

// --- Seam -----------------------------------------------------------------------

std::optional<std::uint32_t> Seam::find(const core::Id& vertex) const {
  const auto it = by_id_.find(vertex);
  if (it == by_id_.end()) return std::nullopt;
  return it->second;
}

std::vector<Triangle> Seam::triangles_facing(const dvec3& n) const {
  if (core::dot(n, normal()) >= 0) return triangles;
  std::vector<Triangle> out = triangles;
  for (Triangle& t : out) std::swap(t[1], t[2]);
  return out;
}

double Seam::area() const {
  double a = 0;
  for (const Triangle& t : triangles) {
    const std::array<dvec2, 3> q = triangle_uv(t);
    a += core::cross(q[1] - q[0], q[2] - q[0]);
  }
  return a / 2.0;
}

dvec3 Seam::to_model(const dvec2& uv) const {
  return curved() ? cylinder_point(frame, radius, uv) : frame.to_model(uv);
}

dvec2 Seam::to_uv(const dvec3& p) const {
  const dvec3 d = p - frame.origin;
  if (!curved()) return {core::dot(d, frame.x_axis), core::dot(d, frame.y_axis)};
  double a = std::atan2(core::dot(d, frame.y_axis), core::dot(d, frame.x_axis));
  const double lo = centre_angle - kPi;
  a = lo + std::fmod(std::fmod(a - lo, 2.0 * kPi) + 2.0 * kPi, 2.0 * kPi);
  if (a >= lo + 2.0 * kPi) a = lo;
  return {radius * a, core::dot(d, frame.normal())};
}

double Seam::offset(const dvec3& p) const {
  const dvec3 n = frame.normal();
  const dvec3 d = p - frame.origin;
  if (!curved()) return core::dot(d, n);
  return core::length(d - n * core::dot(d, n)) - radius;
}

dvec3 Seam::normal_at(const dvec3& p) const {
  const dvec3 n = frame.normal();
  if (!curved()) return n;
  const dvec3 d = p - frame.origin;
  const dvec3 r = d - n * core::dot(d, n);
  const double len = core::length(r);
  return len > 0 ? r / len : frame.x_axis;
}

std::array<dvec2, 3> Seam::triangle_uv(const Triangle& t) const {
  std::array<dvec2, 3> q{vertices[t[0]].uv, vertices[t[1]].uv, vertices[t[2]].uv};
  if (band) {
    const double hi = std::max({q[0].x, q[1].x, q[2].x});
    for (dvec2& p : q) {
      if (p.x < hi - kPi * radius) p.x += 2.0 * kPi * radius;
    }
  }
  return q;
}

std::vector<Triangle> Seam::oriented_triangles(bool along) const {
  if (along) return triangles;
  std::vector<Triangle> out = triangles;
  for (Triangle& t : out) std::swap(t[1], t[2]);
  return out;
}

core::SubjectRef Seam::subject() const {
  return core::SubjectRef{core::SubjectKind::ConnectionSurface, surface, name};
}

void Seam::index() {
  by_id_.clear();
  by_id_.reserve(vertices.size());
  for (std::uint32_t i = 0; i < vertices.size(); ++i) by_id_.emplace(vertices[i].id, i);
}

double derive_spacing(const SeamOutline& outline, const core::ProductSettings& settings) {
  if (outline.loops.empty()) return settings.min_feature_size_mm();
  if (outline.radius > 0) {
    dvec2 lo{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    dvec2 hi = -lo;
    for (const OutlineLoop& loop : outline.loops) {
      for (const dvec2& p : coarse_points(loop)) {
        lo = core::min(lo, p);
        hi = core::max(hi, p);
      }
    }
    // A chord running h round the axis strays h² / 8r from it; the
    // triangulation's edges run up to about 1.5 spacings round it (by the
    // outline, where the lattice keeps half a spacing away).
    const double chord = std::sqrt(8.0 * outline.radius * kSeamDeflection) / 1.5;
    return std::max(std::min(core::length(hi - lo) / 16.0, chord), settings.min_feature_size_mm());
  }
  const std::vector<dvec2> pts = coarse_points(outline.loops[0]);
  if (pts.empty()) return settings.min_feature_size_mm();
  dvec2 lo = pts[0], hi = pts[0];
  for (const dvec2& p : pts) {
    lo = core::min(lo, p);
    hi = core::max(hi, p);
  }
  return std::max(core::length(hi - lo) / 16.0, settings.min_feature_size_mm());
}

namespace {

// A cylinder's seam (L2): the planar construction in unrolled coordinates.
// A band's rectangle is triangulated whole, then its right side (x1) is
// identified with its left (x0), so the seam closes round the axis.
Result<Seam> make_cylinder_seam(const core::SubjectRef& surface, const SeamOutline& outline,
                                double requested) {
  const auto fail = [&](std::string code, std::string message) {
    return Diagnostic::error(std::move(code), std::move(message), {surface});
  };
  const std::optional<Span> span = cylinder_span(outline);
  if (!span) {
    return fail("mesh.seam.outline",
                outline.band ? "A band's outline must be two lines a full turn long"
                             : "A cylinder's outline must be one rectangle in unrolled coordinates");
  }
  const Span& sp = *span;
  const double r = outline.radius;
  // A band's spacing is at most its circumference over kArcPiecesPerTurn,
  // so its lattice always reaches round it: coarser, a tall band had no
  // interior points, and its triangles reached from one side of the cut to
  // the other, more than half a turn round the axis, which triangle_uv()
  // and everything after it can't tell from the short way round (H4).
  const double spacing =
      outline.band ? std::min(requested, sp.width() / kArcPiecesPerTurn) : requested;
  const double estimate = 2.0 * (sp.width() + sp.height()) / spacing +
                          sp.width() * sp.height() / (spacing * spacing * 0.86);
  if (estimate > static_cast<double>(kMaxSeamVertices)) {
    return fail("mesh.seam.too_fine", "A spacing of " + std::to_string(spacing) +
                                          " mm needs about " +
                                          std::to_string(static_cast<long long>(estimate)) +
                                          " seam vertices");
  }

  // The rectangle, counter-clockwise from (x0, y0).
  const dvec2 c00{sp.x0, sp.y0}, c10{sp.x1, sp.y0}, c11{sp.x1, sp.y1}, c01{sp.x0, sp.y1};
  OutlineLoop rect;
  rect.segments = {OutlineSegment::line(c00, c10), OutlineSegment::line(c10, c11),
                   OutlineSegment::line(c11, c01), OutlineSegment::line(c01, c00)};
  std::vector<dvec2> pts;
  Loop domain;
  std::array<std::uint32_t, 5> starts{};  // where each side begins in `domain`, then its size
  for (std::size_t g = 0; g < 4; ++g) {
    const OutlineSegment& s = rect.segments[g];
    starts[g] = static_cast<std::uint32_t>(pts.size());
    const std::size_t n = cylinder_pieces(s, spacing, r);
    for (std::size_t k = 0; k < n; ++k) {
      domain.push_back(static_cast<std::uint32_t>(pts.size()));
      pts.push_back(s.point(static_cast<double>(k) / static_cast<double>(n)));
    }
  }
  starts[4] = static_cast<std::uint32_t>(pts.size());
  const std::size_t rect_points = pts.size();
  for (const dvec2& p : interior_points(pts, {domain}, spacing, 0.5 * spacing)) pts.push_back(p);

  Result<std::vector<Triangle>> tris = triangulate(pts, {domain});
  if (!tris) return with_subject(tris.error(), surface);

  Seam seam;
  seam.surface = surface.id;
  seam.name = surface.label;
  seam.frame = outline.frame;
  seam.spacing = spacing;
  seam.radius = r;
  seam.band = outline.band;
  seam.centre_angle = (sp.x0 + sp.x1) / 2.0 / r;

  // Final vertex order: outline vertices loop by loop, then the rest.
  std::vector<std::uint32_t> final_index(pts.size(), 0xffffffffu);
  std::vector<dvec2> final_uv;
  const auto keep = [&](std::uint32_t i) {
    final_index[i] = static_cast<std::uint32_t>(final_uv.size());
    final_uv.push_back(pts[i]);
  };
  if (!outline.band) {
    SeamLoop loop;
    for (std::uint32_t g = 0; g < 4; ++g) {
      loop.segment_starts.push_back(starts[g]);
    }
    for (std::uint32_t i = 0; i < rect_points; ++i) {
      loop.vertices.push_back(i);
      keep(i);
    }
    seam.loops.push_back(std::move(loop));
    seam.outline = SeamOutline{outline.frame, {rect}, r, false};
  } else {
    // Bottom (x0 → x1 at y0), then top (from (x0, y1), back along y1).
    const std::uint32_t nx = starts[1] - starts[0];
    const std::uint32_t ny = starts[2] - starts[1];
    SeamLoop bottom, top;
    bottom.segment_starts = {0};
    top.segment_starts = {0};
    for (std::uint32_t k = 0; k < nx; ++k) {
      bottom.vertices.push_back(static_cast<std::uint32_t>(final_uv.size()));
      keep(starts[0] + k);
    }
    // The top's first point is (x1, y1), the same point as (x0, y1).
    top.vertices.push_back(static_cast<std::uint32_t>(final_uv.size()));
    keep(starts[3]);
    for (std::uint32_t k = 1; k < nx; ++k) {
      top.vertices.push_back(static_cast<std::uint32_t>(final_uv.size()));
      keep(starts[2] + k);
    }
    // The left side's own points (between the corners); the right side's
    // are the same points.
    for (std::uint32_t k = 1; k < ny; ++k) keep(starts[3] + k);
    final_index[starts[1]] = final_index[starts[0]];  // (x1, y0) is (x0, y0)
    final_index[starts[2]] = final_index[starts[3]];  // (x1, y1) is (x0, y1)
    if (starts[4] - starts[3] != ny) {
      return fail("mesh.seam.outline", "A band's sides were divided differently");
    }
    for (std::uint32_t k = 1; k < ny; ++k) {
      // Right side point k (from y0 up) is left side point ny - k (from y1 down).
      final_index[starts[1] + k] = final_index[starts[3] + (ny - k)];
    }
    seam.loops.push_back(std::move(bottom));
    seam.loops.push_back(std::move(top));
    OutlineLoop lower, upper;
    lower.segments = {OutlineSegment::line(c00, c10)};
    upper.segments = {OutlineSegment::line(c11, c01)};
    seam.outline = SeamOutline{outline.frame, {lower, upper}, r, true};
  }
  seam.outline_vertex_count = 0;
  for (const SeamLoop& l : seam.loops) seam.outline_vertex_count += l.vertices.size();
  for (std::uint32_t i = static_cast<std::uint32_t>(rect_points); i < pts.size(); ++i) keep(i);
  for (const Triangle& t : *tris) {
    const Triangle f{final_index[t[0]], final_index[t[1]], final_index[t[2]]};
    if (f[0] == f[1] || f[1] == f[2] || f[2] == f[0]) {
      return fail("mesh.seam.outline", "The band is too narrow round its axis for its spacing");
    }
    seam.triangles.push_back(f);
  }

  core::Hasher h;
  h.add(std::string_view("vs.mesh.seam.cylinder/1")).add(surface.id);
  h.add(outline.frame.origin.x).add(outline.frame.origin.y).add(outline.frame.origin.z);
  h.add(outline.frame.x_axis.x).add(outline.frame.x_axis.y).add(outline.frame.x_axis.z);
  h.add(outline.frame.y_axis.x).add(outline.frame.y_axis.y).add(outline.frame.y_axis.z);
  h.add(r).add(outline.band ? 1 : 0).add(spacing).add(final_uv.size()).add(seam.outline_vertex_count);
  for (const dvec2& p : final_uv) h.add(p.x).add(p.y);
  seam.key = h.finalize();
  const core::Id base = core::Id::from_hash(seam.key);
  seam.vertices.reserve(final_uv.size());
  for (std::uint32_t i = 0; i < final_uv.size(); ++i) {
    SeamVertex v;
    v.id = core::Id::derive(base, static_cast<std::uint64_t>(i));
    v.uv = final_uv[i];
    v.position = cylinder_point(outline.frame, r, final_uv[i]);
    v.position32 = core::to_fp32(v.position);
    seam.vertices.push_back(v);
  }
  seam.index();
  return seam;
}

}  // namespace

Result<Seam> make_seam(const core::SubjectRef& surface, const SeamOutline& outline,
                       double spacing, const core::ProductSettings& settings) {
  const auto fail = [&](std::string code, std::string message) {
    return Diagnostic::error(std::move(code), std::move(message), {surface});
  };
  if (!std::isfinite(spacing) || spacing <= 0 || spacing < settings.min_feature_size_mm()) {
    return fail("mesh.seam.spacing", "Seam spacing " + std::to_string(spacing) +
                                         " mm is not positive or is below the minimum feature size");
  }
  if (outline.loops.empty()) return fail("mesh.seam.outline", "The surface has no outline");
  for (const OutlineLoop& loop : outline.loops) {
    if (loop.segments.empty()) return fail("mesh.seam.outline", "An outline loop is empty");
  }
  if (outline.radius > 0) return make_cylinder_seam(surface, outline, spacing);

  // Orient: outer counter-clockwise, holes clockwise.
  std::vector<OutlineLoop> loops;
  for (std::size_t i = 0; i < outline.loops.size(); ++i) {
    const bool ccw = coarse_area(outline.loops[i]) > 0;
    const bool want_ccw = i == 0;
    loops.push_back(ccw == want_ccw ? outline.loops[i] : reversed(outline.loops[i]));
  }

  // Estimate the size before dividing anything.
  double perimeter = 0;
  for (const OutlineLoop& loop : loops) {
    for (const OutlineSegment& s : loop.segments) perimeter += s.length();
  }
  const double area = std::abs(coarse_area(loops[0])) / 2.0;
  const double estimate = perimeter / spacing + area / (spacing * spacing * 0.86);
  if (estimate > static_cast<double>(kMaxSeamVertices)) {
    return fail("mesh.seam.too_fine", "A spacing of " + std::to_string(spacing) +
                                          " mm needs about " +
                                          std::to_string(static_cast<long long>(estimate)) +
                                          " seam vertices");
  }

  Seam seam;
  seam.surface = surface.id;
  seam.name = surface.label;
  seam.frame = outline.frame;
  seam.outline = SeamOutline{outline.frame, loops};
  seam.spacing = spacing;

  std::vector<dvec2> pts;
  std::vector<Loop> index_loops;
  for (const OutlineLoop& loop : loops) {
    SeamLoop sl;
    Loop il;
    for (const OutlineSegment& s : loop.segments) {
      sl.segment_starts.push_back(static_cast<std::uint32_t>(sl.vertices.size()));
      const std::size_t n = pieces(s, spacing);
      for (std::size_t k = 0; k < n; ++k) {
        const auto idx = static_cast<std::uint32_t>(pts.size());
        pts.push_back(s.point(static_cast<double>(k) / static_cast<double>(n)));
        sl.vertices.push_back(idx);
        il.push_back(idx);
      }
    }
    seam.loops.push_back(std::move(sl));
    index_loops.push_back(std::move(il));
  }
  seam.outline_vertex_count = pts.size();
  for (const dvec2& p : interior_points(pts, index_loops, spacing, 0.5 * spacing)) pts.push_back(p);

  Result<std::vector<Triangle>> tris = triangulate(pts, index_loops);
  if (!tris) return with_subject(tris.error(), surface);
  seam.triangles = std::move(*tris);
  // Each hole on its own, for volumes whose face has no hole there.
  for (std::size_t h = 1; h < index_loops.size(); ++h) {
    std::vector<dvec2> hp;
    Loop hl;
    for (const std::uint32_t i : index_loops[h]) {
      hl.push_back(static_cast<std::uint32_t>(hp.size()));
      hp.push_back(pts[i]);
    }
    Result<std::vector<Triangle>> fill = triangulate(hp, {hl});
    if (!fill) return with_subject(fill.error(), surface);
    for (Triangle& t : *fill) {
      for (std::uint32_t& v : t) v = index_loops[h][v];
    }
    seam.hole_fills.push_back(std::move(*fill));
  }

  core::Hasher h;
  h.add(std::string_view("vs.mesh.seam/1")).add(surface.id);
  h.add(outline.frame.origin.x).add(outline.frame.origin.y).add(outline.frame.origin.z);
  h.add(outline.frame.x_axis.x).add(outline.frame.x_axis.y).add(outline.frame.x_axis.z);
  h.add(outline.frame.y_axis.x).add(outline.frame.y_axis.y).add(outline.frame.y_axis.z);
  h.add(spacing).add(pts.size()).add(seam.outline_vertex_count);
  for (const dvec2& p : pts) h.add(p.x).add(p.y);
  seam.key = h.finalize();
  const core::Id base = core::Id::from_hash(seam.key);

  seam.vertices.reserve(pts.size());
  for (std::uint32_t i = 0; i < pts.size(); ++i) {
    SeamVertex v;
    v.id = core::Id::derive(base, static_cast<std::uint64_t>(i));
    v.uv = pts[i];
    v.position = outline.frame.to_model(pts[i]);
    v.position32 = core::to_fp32(v.position);
    seam.vertices.push_back(v);
  }
  seam.index();
  return seam;
}

std::vector<std::uint32_t> add_seam_vertices(Mesh& mesh, const Seam& seam) {
  std::vector<std::uint32_t> map;
  map.reserve(seam.vertices.size());
  for (const SeamVertex& v : seam.vertices) map.push_back(mesh.add_vertex(v.position32, v.id));
  return map;
}

void add_seam_face(Mesh& mesh, const Seam& seam, const std::vector<std::uint32_t>& vertex_map,
                   const dvec3& outward, const core::Id& volume) {
  for (const Triangle& t : seam.triangles_facing(outward)) {
    mesh.add_triangle(vertex_map[t[0]], vertex_map[t[1]], vertex_map[t[2]], volume);
  }
}

std::size_t attach_seam_vertices(Mesh& mesh, std::span<const Seam* const> seams) {
  using Key = std::array<std::uint32_t, 3>;
  // Bit patterns, with -0 as +0: equal floats, never nearby ones (H4).
  const auto bits = [](float f) { return std::bit_cast<std::uint32_t>(f == 0.0f ? 0.0f : f); };
  const auto key = [&](const fvec3& p) { return Key{bits(p.x), bits(p.y), bits(p.z)}; };
  std::map<Key, core::Id> by_position;
  for (const Seam* seam : seams) {
    for (const SeamVertex& v : seam->vertices) by_position.emplace(key(v.position32), v.id);
  }
  std::size_t named = 0;
  for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
    if (!mesh.seam_vertices[i].is_null()) continue;
    const auto it = by_position.find(key(mesh.positions[i]));
    if (it == by_position.end()) continue;
    mesh.seam_vertices[i] = it->second;
    ++named;
  }
  return named;
}

}  // namespace vs::mesh

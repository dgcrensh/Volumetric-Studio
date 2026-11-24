#include "vs/sketch/offset.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <optional>

#include <fmt/format.h>

namespace vs::sketch {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr std::size_t kNone = static_cast<std::size_t>(-1);
// How many times a Bézier's offset may be halved before giving up (4,096
// pieces).
constexpr int kMaxFitDepth = 12;

dvec2 perp(const dvec2& v) { return {-v.y, v.x}; }

// Angle in [0, 2π).
double wrap_positive(double a) {
  a = std::fmod(a, kTwoPi);
  if (a < 0.0) a += kTwoPi;
  return a;
}

Diagnostic error(const Sketch& sketch, std::string code, std::string message,
                 const Id& subject = {}) {
  std::vector<core::SubjectRef> subjects;
  if (!subject.is_null()) subjects.push_back(sketch.subject(subject));
  subjects.push_back(sketch.sketch_subject());
  return Diagnostic::error(std::move(code), std::move(message), std::move(subjects));
}

// --- Bézier helpers -----------------------------------------------------

std::array<BezierGeometry, 2> split(const BezierGeometry& b, double t) {
  const double u = 1.0 - t;
  const dvec2 p01 = b.p[0] * u + b.p[1] * t;
  const dvec2 p12 = b.p[1] * u + b.p[2] * t;
  const dvec2 p23 = b.p[2] * u + b.p[3] * t;
  const dvec2 p012 = p01 * u + p12 * t;
  const dvec2 p123 = p12 * u + p23 * t;
  const dvec2 m = p012 * u + p123 * t;
  return {BezierGeometry{{b.p[0], p01, p012, m}}, BezierGeometry{{m, p123, p23, b.p[3]}}};
}

BezierGeometry sub_bezier(const BezierGeometry& b, double t0, double t1) {
  if (t0 <= 0.0 && t1 >= 1.0) return b;
  BezierGeometry left = t1 >= 1.0 ? b : split(b, t1)[0];
  if (t0 <= 0.0) return left;
  return split(left, t0 / t1)[1];
}

dvec2 second_derivative(const BezierGeometry& b, double t) {
  const dvec2 a = b.p[0] - b.p[1] * 2.0 + b.p[2];
  const dvec2 c = b.p[1] - b.p[2] * 2.0 + b.p[3];
  return (a * (1.0 - t) + c * t) * 6.0;
}

BezierGeometry reversed(const BezierGeometry& b) { return {{b.p[3], b.p[2], b.p[1], b.p[0]}}; }

// --- pieces of a chain --------------------------------------------------

// One curve of a chain, oriented along it: a line, an arc (signed sweep:
// positive counter-clockwise) or a Bézier, parametrised on [0, 1].
struct Piece {
  enum class Kind { Line, Arc, Bezier };
  Kind kind = Kind::Line;
  dvec2 a{}, b{};  // line
  dvec2 c{};       // arc
  double r = 0.0, a0 = 0.0, sweep = 0.0;
  Id center;  // the point the arc's centre reuses; null: a new point
  BezierGeometry bz{};
  std::size_t source = kNone;  // index in the chain; kNone: a corner arc
  bool construction = false;

  [[nodiscard]] dvec2 at(double t) const {
    switch (kind) {
      case Kind::Line: return a + (b - a) * t;
      case Kind::Arc: {
        const double th = a0 + sweep * t;
        return c + dvec2{std::cos(th), std::sin(th)} * r;
      }
      case Kind::Bezier: return bz.at(t);
    }
    return {};
  }
  [[nodiscard]] dvec2 derivative(double t) const {
    switch (kind) {
      case Kind::Line: return b - a;
      case Kind::Arc: {
        const double th = a0 + sweep * t;
        return dvec2{-std::sin(th), std::cos(th)} * (r * sweep);
      }
      case Kind::Bezier: return bz.derivative(t);
    }
    return {};
  }
  [[nodiscard]] dvec2 start() const { return at(0.0); }
  [[nodiscard]] dvec2 end() const { return at(1.0); }
  [[nodiscard]] dvec2 tangent_start() const {
    return kind == Kind::Bezier ? bz.tangent(false) : core::normalize(derivative(0.0));
  }
  [[nodiscard]] dvec2 tangent_end() const {
    return kind == Kind::Bezier ? bz.tangent(true) : core::normalize(derivative(1.0));
  }
  // Keeps the part from t0 to t1.
  void keep(double t0, double t1) {
    switch (kind) {
      case Kind::Line: {
        const dvec2 s = at(t0), e = at(t1);
        a = s;
        b = e;
        break;
      }
      case Kind::Arc:
        a0 += sweep * t0;
        sweep *= (t1 - t0);
        break;
      case Kind::Bezier: bz = sub_bezier(bz, t0, t1); break;
    }
  }
  // ∫ (x dy − y dx) / 2 along the piece.
  [[nodiscard]] double area_term() const {
    switch (kind) {
      case Kind::Line: return 0.5 * core::cross(a, b);
      case Kind::Arc: {
        const double t0 = a0, t1 = a0 + sweep;
        return 0.5 * (r * r * sweep + r * c.x * (std::sin(t1) - std::sin(t0)) -
                      r * c.y * (std::cos(t1) - std::cos(t0)));
      }
      case Kind::Bezier: return bz.area_term();
    }
    return 0.0;
  }
  // Points along the piece for crossing tests.
  [[nodiscard]] int samples() const {
    switch (kind) {
      case Kind::Line: return 1;
      case Kind::Arc:
        return std::max(2, static_cast<int>(std::ceil(std::abs(sweep) / (std::numbers::pi / 32))));
      case Kind::Bezier: return 24;
    }
    return 1;
  }
};

// The ends of a chain curve in its own direction (circles have none).
struct Ends {
  Id start;
  Id end;
};

std::optional<Ends> curve_ends(const Entity& e) {
  if (const auto* l = e.as<Line>()) return Ends{l->start, l->end};
  if (const auto* a = e.as<Arc>()) return Ends{a->start, a->end};
  if (const auto* b = e.as<Bezier>()) return Ends{b->start, b->end};
  return std::nullopt;
}

// Point ids joined by Coincident constraints share one representative.
class PointClasses {
 public:
  explicit PointClasses(const Sketch& sketch) {
    for (const auto& c : sketch.constraints()) {
      if (c.kind != ConstraintKind::Coincident || c.refs.size() != 2) continue;
      if (!sketch.point(c.refs[0]) || !sketch.point(c.refs[1])) continue;
      const Id a = find(c.refs[0]);
      const Id b = find(c.refs[1]);
      if (a != b) parent_[std::max(a, b)] = std::min(a, b);
    }
  }
  Id find(Id id) const {
    for (auto it = parent_.find(id); it != parent_.end(); it = parent_.find(id)) id = it->second;
    return id;
  }

 private:
  std::map<Id, Id> parent_;
};

// The source chain's curve as a piece, in travel order.
Piece source_piece(const Sketch& sketch, const Id& curve, bool rev) {
  Piece p;
  const Entity& e = *sketch.find_entity(curve);
  p.construction = e.construction;
  if (const auto lg = line_geometry(sketch, curve)) {
    p.kind = Piece::Kind::Line;
    p.a = rev ? lg->end : lg->start;
    p.b = rev ? lg->start : lg->end;
  } else if (const auto ag = arc_geometry(sketch, curve)) {
    p.kind = Piece::Kind::Arc;
    p.c = ag->center;
    p.r = ag->radius;
    p.center = e.as<Arc>()->center;
    p.a0 = rev ? ag->end_angle : ag->start_angle;
    p.sweep = rev ? ag->start_angle - ag->end_angle : ag->end_angle - ag->start_angle;
  } else if (const auto bg = bezier_geometry(sketch, curve)) {
    p.kind = Piece::Kind::Bezier;
    p.bz = rev ? reversed(*bg) : *bg;
  }
  return p;
}

// --- Bézier offsets -----------------------------------------------------

class BezierOffset {
 public:
  BezierOffset(const BezierGeometry& b, double d, double tol) : b_(b), d_(d), tol_(tol) {
    scale_ = 0.0;
    for (const auto& q : b.p) scale_ = std::max(scale_, core::length(q - b.p[0]));
  }

  // The unit left normal at t.
  [[nodiscard]] dvec2 normal(double t) const {
    const dvec2 v = b_.derivative(t);
    if (core::length(v) > 1e-12 * std::max(scale_, 1.0)) return perp(core::normalize(v));
    if (t <= 0.0) return perp(b_.tangent(false));
    if (t >= 1.0) return perp(b_.tangent(true));
    return perp(core::normalize(b_.at(std::min(1.0, t + 1e-7)) - b_.at(std::max(0.0, t - 1e-7))));
  }
  [[nodiscard]] dvec2 at(double t) const { return b_.at(t) + normal(t) * d_; }
  [[nodiscard]] dvec2 derivative(double t) const {
    const dvec2 v = b_.derivative(t);
    const double s = core::length(v);
    if (s > 1e-9 * std::max(scale_, 1.0)) {
      const double k = core::cross(v, second_derivative(b_, t)) / (s * s * s);
      return v * (1.0 - d_ * k);
    }
    // Where a handle has no length the offset has none either.
    return {};
  }
  // Whether the offset folds somewhere (d at or past the radius of
  // curvature on the side it moves to).
  [[nodiscard]] bool folds() const {
    constexpr int kSamples = 256;
    for (int i = 0; i <= kSamples; ++i) {
      const double t = static_cast<double>(i) / kSamples;
      const dvec2 v = b_.derivative(t);
      const double s = core::length(v);
      if (s <= 1e-9 * std::max(scale_, 1.0)) continue;
      const double k = core::cross(v, second_derivative(b_, t)) / (s * s * s);
      if (1.0 - d_ * k <= 1e-6) return true;
    }
    return false;
  }
  // Fits [0, 1]; false when it can't within kMaxFitDepth halvings.
  bool fit(std::vector<BezierGeometry>& out) const { return fit(0.0, 1.0, 0, out); }

 private:
  bool fit(double t0, double t1, int depth, std::vector<BezierGeometry>& out) const {
    const double h = t1 - t0;
    BezierGeometry f;
    f.p[0] = at(t0);
    f.p[3] = at(t1);
    f.p[1] = f.p[0] + derivative(t0) * (h / 3.0);
    f.p[2] = f.p[3] - derivative(t1) * (h / 3.0);
    if (error(f, t0, t1) <= tol_) {
      out.push_back(f);
      return true;
    }
    if (depth >= kMaxFitDepth) return false;
    const double m = 0.5 * (t0 + t1);
    return fit(t0, m, depth + 1, out) && fit(m, t1, depth + 1, out);
  }

  // The largest distance from the fit to the true offset, by points on the
  // fit projected onto the source.
  [[nodiscard]] double error(const BezierGeometry& f, double t0, double t1) const {
    constexpr int kSamples = 9;
    double worst = 0.0;
    for (int i = 1; i < kSamples; ++i) {
      const double s = static_cast<double>(i) / kSamples;
      const dvec2 q = f.at(s);
      double t = t0 + (t1 - t0) * s;
      for (int it = 0; it < 8; ++it) {
        const dvec2 e = b_.at(t) - q;
        const dvec2 v = b_.derivative(t);
        const double g = core::dot(e, v);
        const double gg = core::dot(v, v) + core::dot(e, second_derivative(b_, t));
        if (!(std::abs(gg) > 0.0)) break;
        t = std::clamp(t - g / gg, 0.0, 1.0);
      }
      const dvec2 e = q - b_.at(t);
      const double dist = core::length(e);
      // On the wrong side counts as the whole distance across.
      const double side = core::cross(b_.derivative(t), e);
      const double err = (side * d_ < 0.0) ? dist + std::abs(d_) : std::abs(dist - std::abs(d_));
      // A point off the end of the source (projected to an end) is judged
      // against the offset point there.
      const double judged = (t <= 0.0 || t >= 1.0) ? std::min(err, core::length(q - at(t))) : err;
      worst = std::max(worst, judged);
      if (!std::isfinite(worst)) return std::numeric_limits<double>::infinity();
    }
    return worst;
  }

  BezierGeometry b_;
  double d_;
  double tol_;
  double scale_;
};

// --- intersections ------------------------------------------------------

struct Carrier {
  bool circle = false;
  dvec2 p{}, dir{};  // line through p along dir
  dvec2 c{};
  double r = 0.0;
};

Carrier carrier(const Piece& p) {
  Carrier out;
  if (p.kind == Piece::Kind::Line) {
    out.p = p.a;
    out.dir = p.b - p.a;
  } else {
    out.circle = true;
    out.c = p.c;
    out.r = p.r;
  }
  return out;
}

std::vector<dvec2> intersect(const Carrier& x, const Carrier& y) {
  std::vector<dvec2> out;
  if (!x.circle && !y.circle) {
    const double den = core::cross(x.dir, y.dir);
    if (std::abs(den) <= 1e-15 * core::length(x.dir) * core::length(y.dir)) return out;
    const double s = core::cross(y.p - x.p, y.dir) / den;
    out.push_back(x.p + x.dir * s);
    return out;
  }
  if (x.circle != y.circle) {
    const Carrier& l = x.circle ? y : x;
    const Carrier& c = x.circle ? x : y;
    const dvec2 u = core::normalize(l.dir);
    const dvec2 f = l.p - c.c;
    const double bq = core::dot(f, u);
    const double disc = bq * bq - (core::dot(f, f) - c.r * c.r);
    if (disc < 0.0) return out;
    const double sq = std::sqrt(disc);
    out.push_back(l.p + u * (-bq - sq));
    if (sq > 0.0) out.push_back(l.p + u * (-bq + sq));
    return out;
  }
  const dvec2 dc = y.c - x.c;
  const double dd = core::length(dc);
  if (dd <= 0.0 || dd > x.r + y.r || dd < std::abs(x.r - y.r)) return out;
  const double a = (x.r * x.r - y.r * y.r + dd * dd) / (2.0 * dd);
  const double hh = std::max(0.0, x.r * x.r - a * a);
  const dvec2 u = dc / dd;
  const dvec2 m = x.c + u * a;
  const double h = std::sqrt(hh);
  out.push_back(m + perp(u) * h);
  if (h > 0.0) out.push_back(m - perp(u) * h);
  return out;
}

// Extends a piece's end (or start) to `x`, a point on its carrier; false
// when that would shorten it.
bool extend_end(Piece& p, const dvec2& x, double tol) {
  if (p.kind == Piece::Kind::Line) {
    if (core::dot(x - p.b, p.b - p.a) < -tol * core::length(p.b - p.a)) return false;
    p.b = x;
    return true;
  }
  const double ang = std::atan2(x.y - p.c.y, x.x - p.c.x);
  const double s = p.sweep > 0.0 ? wrap_positive(ang - p.a0) : -wrap_positive(p.a0 - ang);
  if (std::abs(s) < std::abs(p.sweep) - 1e-12 || std::abs(s) >= kTwoPi - 1e-9) return false;
  p.sweep = s;
  return true;
}

bool extend_start(Piece& p, const dvec2& x, double tol) {
  if (p.kind == Piece::Kind::Line) {
    if (core::dot(p.a - x, p.b - p.a) < -tol * core::length(p.b - p.a)) return false;
    p.a = x;
    return true;
  }
  const double ang = std::atan2(x.y - p.c.y, x.x - p.c.x);
  const double e = p.a0 + p.sweep;
  const double s = p.sweep > 0.0 ? wrap_positive(e - ang) : -wrap_positive(ang - e);
  if (std::abs(s) < std::abs(p.sweep) - 1e-12 || std::abs(s) >= kTwoPi - 1e-9) return false;
  p.sweep = s;
  p.a0 = e - s;
  return true;
}

// Where `p` (near its end) crosses `q` (near its start): parameters on each,
// the pair nearest the joint between them.
std::optional<std::pair<double, double>> crossing(const Piece& p, const Piece& q) {
  const auto sample = [](const Piece& x) {
    const int n = x.samples();
    std::vector<dvec2> out;
    for (int i = 0; i <= n; ++i) out.push_back(x.at(static_cast<double>(i) / n));
    return out;
  };
  const std::vector<dvec2> pp = sample(p), qq = sample(q);
  const double n = static_cast<double>(pp.size() - 1);
  const double m = static_cast<double>(qq.size() - 1);
  std::optional<std::pair<double, double>> best;
  double best_cost = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i + 1 < pp.size(); ++i) {
    const dvec2 a = pp[i], ab = pp[i + 1] - pp[i];
    for (std::size_t j = 0; j + 1 < qq.size(); ++j) {
      const dvec2 c = qq[j], cd = qq[j + 1] - qq[j];
      const double den = core::cross(ab, cd);
      if (den == 0.0) continue;
      const double u = core::cross(c - a, cd) / den;
      const double v = core::cross(c - a, ab) / den;
      constexpr double kSlack = 1e-9;
      if (u < -kSlack || u > 1.0 + kSlack || v < -kSlack || v > 1.0 + kSlack) continue;
      const double s = (static_cast<double>(i) + std::clamp(u, 0.0, 1.0)) / n;
      const double t = (static_cast<double>(j) + std::clamp(v, 0.0, 1.0)) / m;
      const double cost = (1.0 - s) + t;
      if (cost < best_cost) {
        best_cost = cost;
        best = std::pair{s, t};
      }
    }
  }
  if (!best) return best;
  // Newton on p(s) = q(t).
  auto [s, t] = *best;
  for (int it = 0; it < 20; ++it) {
    const dvec2 f = p.at(s) - q.at(t);
    const dvec2 ps = p.derivative(s), qt = q.derivative(t);
    const double det = core::cross(ps, qt * -1.0);
    if (det == 0.0) break;
    // [ps  -qt] [ds dt]^T = -f
    const double ds = core::cross(f * -1.0, qt * -1.0) / det;
    const double dt = core::cross(ps, f * -1.0) / det;
    s = std::clamp(s + ds, 0.0, 1.0);
    t = std::clamp(t + dt, 0.0, 1.0);
    if (std::abs(ds) + std::abs(dt) < 1e-15) break;
  }
  return std::pair{s, t};
}

// Whether two segments cross or touch.
bool segments_meet(const dvec2& a, const dvec2& b, const dvec2& c, const dvec2& d) {
  const auto orient = [](const dvec2& p, const dvec2& q, const dvec2& r) {
    const double v = core::cross(q - p, r - p);
    return (v > 0.0) - (v < 0.0);
  };
  const int o1 = orient(a, b, c), o2 = orient(a, b, d);
  const int o3 = orient(c, d, a), o4 = orient(c, d, b);
  if (o1 * o2 < 0 && o3 * o4 < 0) return true;
  const auto on = [](const dvec2& p, const dvec2& q, const dvec2& r) {
    return std::min(p.x, q.x) <= r.x && r.x <= std::max(p.x, q.x) && std::min(p.y, q.y) <= r.y &&
           r.y <= std::max(p.y, q.y);
  };
  return (o1 == 0 && on(a, b, c)) || (o2 == 0 && on(a, b, d)) || (o3 == 0 && on(c, d, a)) ||
         (o4 == 0 && on(c, d, b));
}

// Whether a chain of pieces crosses itself, judged on sampled points.
bool self_crossing(const std::vector<Piece>& pieces, bool closed) {
  struct Seg {
    dvec2 a, b;
    std::size_t index;  // in the whole polyline
  };
  std::vector<Seg> segs;
  for (const auto& p : pieces) {
    const int n = p.samples();
    dvec2 prev = p.start();
    for (int i = 1; i <= n; ++i) {
      const dvec2 next = p.at(static_cast<double>(i) / n);
      segs.push_back({prev, next, segs.size()});
      prev = next;
    }
  }
  const std::size_t n = segs.size();
  std::sort(segs.begin(), segs.end(), [](const Seg& x, const Seg& y) {
    return std::min(x.a.x, x.b.x) < std::min(y.a.x, y.b.x);
  });
  for (std::size_t i = 0; i < n; ++i) {
    const double xmax = std::max(segs[i].a.x, segs[i].b.x);
    for (std::size_t j = i + 1; j < n && std::min(segs[j].a.x, segs[j].b.x) <= xmax; ++j) {
      const std::size_t a = segs[i].index, b = segs[j].index;
      const std::size_t gap = a > b ? a - b : b - a;
      if (gap <= 1 || (closed && gap == n - 1)) continue;
      if (segments_meet(segs[i].a, segs[i].b, segs[j].a, segs[j].b)) return true;
    }
  }
  return false;
}

double chain_area(const std::vector<Piece>& pieces) {
  double a = 0.0;
  for (const auto& p : pieces) a += p.area_term();
  return a;
}

// --- one chain ----------------------------------------------------------

class ChainOffsetter {
 public:
  ChainOffsetter(const Sketch& sketch, const OffsetChain& chain, const OffsetOptions& options,
                 const core::ProductSettings& settings)
      : sketch_(sketch), chain_(chain), options_(options), settings_(settings) {}

  Result<std::vector<Piece>> run() {
    const std::size_t n = chain_.curves.size();
    std::vector<Piece> source;
    for (std::size_t i = 0; i < n; ++i) {
      source.push_back(source_piece(sketch_, chain_.curves[i], chain_.reversed[i]));
      source.back().source = i;
    }
    // Left distance: a counter-clockwise loop's outside is on its right.
    double dl = options_.distance;
    if (chain_.closed && chain_area(source) > 0.0) dl = -dl;
    dl_ = dl;

    // Each curve's offset, as one or more pieces.
    std::vector<std::vector<Piece>> groups(n);
    for (std::size_t i = 0; i < n; ++i) {
      auto g = offset_piece(source[i]);
      if (!g.ok()) return g.error();
      groups[i] = std::move(g.value());
    }

    // Corners.
    const std::size_t joints = chain_.closed ? n : n - 1;
    for (std::size_t k = 0; k < joints; ++k) {
      const std::size_t i = k, j = (k + 1) % n;
      if (auto d = join(source[i], groups[i], groups[j]); !d.empty()) return d[0];
      if (groups[i].empty() || groups[j].empty()) return collapsed(groups[i].empty() ? i : j);
    }

    std::vector<Piece> out;
    for (auto& g : groups)
      for (auto& p : g) out.push_back(std::move(p));
    for (const auto& p : out) {
      const std::size_t s = p.source == kNone ? 0 : p.source;
      if (core::distance(p.start(), p.end()) < settings_.min_feature_size_mm() &&
          !(p.kind == Piece::Kind::Arc && std::abs(p.sweep) > std::numbers::pi))
        return collapsed(s);
    }
    if (self_crossing(out, chain_.closed)) {
      return error(sketch_, "sketch.offset.self_crossing",
                   fmt::format("The offset of {} by {} mm would cross itself",
                               sketch_.label(chain_.curves[0]), std::abs(options_.distance)),
                   chain_.curves[0]);
    }
    if (chain_.closed) {
      const double before = chain_area(source);
      const double after = chain_area(out);
      const bool grows = options_.distance > 0.0;
      if (before * after <= 0.0 || (std::abs(after) > std::abs(before)) != grows) {
        return error(sketch_, "sketch.offset.self_crossing",
                     fmt::format("The offset of the loop through {} by {} mm would turn it inside out",
                                 sketch_.label(chain_.curves[0]), std::abs(options_.distance)),
                     chain_.curves[0]);
      }
    }
    return out;
  }

 private:
  Diagnostic collapsed(std::size_t i) const {
    const Id& id = chain_.curves[std::min(i, chain_.curves.size() - 1)];
    return error(sketch_, "sketch.offset.collapsed",
                 fmt::format("{} has no offset {} mm to that side: it would collapse or fold",
                             sketch_.label(id), std::abs(options_.distance)),
                 id);
  }

  Result<std::vector<Piece>> offset_piece(const Piece& s) const {
    const double minf = settings_.min_feature_size_mm();
    std::vector<Piece> out;
    switch (s.kind) {
      case Piece::Kind::Line: {
        if (core::distance(s.a, s.b) < minf) return collapsed(s.source);
        const dvec2 nrm = perp(core::normalize(s.b - s.a));
        Piece p = s;
        p.a = s.a + nrm * dl_;
        p.b = s.b + nrm * dl_;
        out.push_back(p);
        break;
      }
      case Piece::Kind::Arc: {
        // The left of a counter-clockwise arc is its centre.
        const double r = s.r - (s.sweep > 0.0 ? dl_ : -dl_);
        if (r < minf) return collapsed(s.source);
        Piece p = s;
        p.r = r;
        out.push_back(p);
        break;
      }
      case Piece::Kind::Bezier: {
        const BezierOffset off(s.bz, dl_, minf);
        std::vector<BezierGeometry> fits;
        if (off.folds() || !off.fit(fits)) return collapsed(s.source);
        for (const auto& f : fits) {
          Piece p = s;
          p.bz = f;
          out.push_back(p);
        }
        break;
      }
    }
    return out;
  }

  // Joins the end of `g` to the start of `h` at the source's end point.
  std::vector<Diagnostic> join(const Piece& src, std::vector<Piece>& g, std::vector<Piece>& h) {
    const dvec2 e = g.back().end();
    const dvec2 s = h.front().start();
    const double tol = settings_.linear_tolerance_mm;
    if (core::distance(e, s) <= tol) return {};
    const dvec2 v = src.end();  // the corner
    const double turn = core::cross(g.back().tangent_end(), h.front().tangent_start());
    const bool outside = dl_ * turn <= 0.0;
    if (outside) {
      if (options_.corners == OffsetCorners::Extend && g.back().kind != Piece::Kind::Bezier &&
          h.front().kind != Piece::Kind::Bezier && extend(v, g.back(), h.front()))
        return {};
      g.push_back(corner_arc(v, e, s, turn, g.back()));
      return {};
    }
    // Inside: trim both back to where they cross, nearest the corner first.
    const std::size_t depth = 8;
    for (std::size_t sum = 0; sum < 2 * depth; ++sum) {
      for (std::size_t a = 0; a <= sum && a < g.size() && a < depth; ++a) {
        const std::size_t b = sum - a;
        if (b >= h.size() || b >= depth) continue;
        const std::size_t gi = g.size() - 1 - a;
        const auto x = crossing(g[gi], h[b]);
        if (!x) continue;
        g.resize(gi + 1);
        g.back().keep(0.0, x->first);
        h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(b));
        h.front().keep(x->second, 1.0);
        return {};
      }
    }
    return {collapsed(src.source)};
  }

  bool extend(const dvec2& v, Piece& p, Piece& q) const {
    const auto xs = intersect(carrier(p), carrier(q));
    std::vector<dvec2> order(xs.begin(), xs.end());
    std::sort(order.begin(), order.end(), [&](const dvec2& x, const dvec2& y) {
      return core::distance(x, v) < core::distance(y, v);
    });
    for (const auto& x : order) {
      Piece pp = p, qq = q;
      if (extend_end(pp, x, settings_.linear_tolerance_mm) &&
          extend_start(qq, x, settings_.linear_tolerance_mm)) {
        p = pp;
        q = qq;
        return true;
      }
    }
    return false;
  }

  Piece corner_arc(const dvec2& v, const dvec2& e, const dvec2& s, double turn,
                   const Piece& before) const {
    Piece p;
    p.kind = Piece::Kind::Arc;
    p.c = v;
    p.r = std::abs(dl_);
    p.a0 = std::atan2(e.y - v.y, e.x - v.x);
    const double to = std::atan2(s.y - v.y, s.x - v.x);
    p.sweep = turn >= 0.0 ? wrap_positive(to - p.a0) : -wrap_positive(p.a0 - to);
    p.construction = before.construction;
    return p;
  }

  const Sketch& sketch_;
  const OffsetChain& chain_;
  const OffsetOptions& options_;
  const core::ProductSettings& settings_;
  double dl_ = 0.0;
};

}  // namespace

// --- chains -------------------------------------------------------------

Result<std::vector<OffsetChain>> offset_chains(const Sketch& sketch, std::span<const Id> curves) {
  if (curves.empty()) {
    return error(sketch, "sketch.offset.empty", "Select the curves to offset");
  }
  const PointClasses classes(sketch);
  // Selected curves in sketch order, without repeats.
  std::vector<Id> order;
  for (const auto& e : sketch.entities()) {
    if (std::find(curves.begin(), curves.end(), e.id) != curves.end()) order.push_back(e.id);
  }
  for (const auto& id : curves) {
    const Entity* e = sketch.find_entity(id);
    if (e == nullptr || e->kind() == EntityKind::Point) {
      return error(sketch, "sketch.offset.not_curve",
                   fmt::format("Only lines, arcs, circles and curves can be offset, not {}",
                               sketch.label(id)),
                   id);
    }
  }

  std::vector<OffsetChain> chains;
  std::map<Id, std::vector<Id>> at;  // point class -> curves meeting there
  std::map<Id, Ends> ends;
  for (const auto& id : order) {
    const Entity& e = *sketch.find_entity(id);
    if (e.as<Circle>() != nullptr) continue;
    const auto ce = curve_ends(e);
    if (!ce) continue;
    const Ends cls{classes.find(ce->start), classes.find(ce->end)};
    ends[id] = cls;
    at[cls.start].push_back(id);
    at[cls.end].push_back(id);
  }
  for (const auto& [point, list] : at) {
    if (list.size() > 2) {
      return error(sketch, "sketch.offset.branching",
                   fmt::format("{} curves meet at {}: offset one branch at a time", list.size(),
                               sketch.label(point)),
                   point);
    }
  }

  std::map<Id, bool> used;
  // Follows the chain from `first`, leaving it at `from`, until it ends or
  // comes back round.
  const auto walk = [&](const Id& first, const Id& from) {
    OffsetChain chain;
    Id curve = first;
    Id here = from;
    for (;;) {
      used[curve] = true;
      const Ends& ce = ends[curve];
      const bool rev = ce.start != here;
      chain.curves.push_back(curve);
      chain.reversed.push_back(rev);
      here = rev ? ce.start : ce.end;
      Id next;
      for (const auto& c : at[here])
        if (!used[c]) next = c;
      if (next.is_null()) break;
      curve = next;
    }
    chain.closed = here == from;
    return chain;
  };

  // Open chains, from the loose end that comes first in sketch order.
  for (const auto& id : order) {
    if (used[id] || !ends.contains(id)) continue;
    const Ends& ce = ends[id];
    if (ce.start == ce.end) continue;
    if (at[ce.start].size() == 1) {
      chains.push_back(walk(id, ce.start));
    } else if (at[ce.end].size() == 1) {
      chains.push_back(walk(id, ce.end));
    }
  }
  // Loops and circles.
  for (const auto& id : order) {
    if (sketch.find_entity(id)->as<Circle>() != nullptr) {
      chains.push_back(OffsetChain{{id}, {false}, true});
      continue;
    }
    if (used[id] || !ends.contains(id)) continue;
    chains.push_back(walk(id, ends[id].start));
  }
  return chains;
}

std::vector<Id> connected_curves(const Sketch& sketch, std::span<const Id> ids) {
  const PointClasses classes(sketch);
  std::map<Id, std::vector<Id>> at;
  for (const auto& e : sketch.entities()) {
    if (const auto ce = curve_ends(e)) {
      at[classes.find(ce->start)].push_back(e.id);
      at[classes.find(ce->end)].push_back(e.id);
    }
  }
  std::map<Id, bool> in;
  std::vector<Id> todo;
  for (const auto& id : ids) {
    const Entity* e = sketch.find_entity(id);
    if (e == nullptr || e->kind() == EntityKind::Point || in[id]) continue;
    in[id] = true;
    todo.push_back(id);
  }
  while (!todo.empty()) {
    const Id id = todo.back();
    todo.pop_back();
    const auto ce = curve_ends(*sketch.find_entity(id));
    if (!ce) continue;
    for (const Id& p : {classes.find(ce->start), classes.find(ce->end)}) {
      const auto& list = at[p];
      if (list.size() != 2) continue;
      for (const auto& c : list) {
        if (!in[c]) {
          in[c] = true;
          todo.push_back(c);
        }
      }
    }
  }
  std::vector<Id> out;
  for (const auto& e : sketch.entities())
    if (in[e.id]) out.push_back(e.id);
  return out;
}

// --- offsetting ---------------------------------------------------------

Result<SketchOffset> offset_curves(Sketch& sketch, std::span<const Id> curves,
                                   const OffsetOptions& options,
                                   const core::ProductSettings& settings) {
  if (!std::isfinite(options.distance) ||
      std::abs(options.distance) < settings.min_feature_size_mm()) {
    return error(sketch, "sketch.offset.distance",
                 fmt::format("The offset distance must be at least {} mm",
                             settings.min_feature_size_mm()));
  }
  auto chains = offset_chains(sketch, curves);
  if (!chains.ok()) return chains.error();

  // Work out every chain before touching the sketch.
  struct Planned {
    const OffsetChain* chain;
    std::vector<Piece> pieces;
    double circle_radius = 0.0;
    std::vector<Id> corner_points;  // the source point at each joint
  };
  std::vector<Planned> planned;
  for (const auto& chain : chains.value()) {
    const Entity& first = *sketch.find_entity(chain.curves[0]);
    if (const auto* c = first.as<Circle>()) {
      const double r = c->radius + options.distance;
      if (r < settings.min_feature_size_mm()) {
        return error(sketch, "sketch.offset.collapsed",
                     fmt::format("{} has no offset {} mm inward: it would collapse",
                                 sketch.label(first.id), std::abs(options.distance)),
                     first.id);
      }
      planned.push_back({&chain, {}, r, {}});
      continue;
    }
    ChainOffsetter offsetter(sketch, chain, options, settings);
    // Corner arcs are centred on the source's joint points: the end of the
    // source curve before each.
    std::vector<Id> joints;
    for (std::size_t i = 0; i < chain.curves.size(); ++i) {
      const auto ce = curve_ends(*sketch.find_entity(chain.curves[i]));
      joints.push_back(chain.reversed[i] ? ce->start : ce->end);
    }
    auto pieces = offsetter.run();
    if (!pieces.ok()) return pieces.error();
    planned.push_back({&chain, std::move(pieces.value()), 0.0, std::move(joints)});
  }

  // Write them.
  Sketch out = sketch;
  SketchOffset result;
  for (const auto& p : planned) {
    const OffsetChain& chain = *p.chain;
    if (p.pieces.empty()) {
      const Entity& src = *sketch.find_entity(chain.curves[0]);
      result.curves.push_back(
          out.add_circle(src.as<Circle>()->center, p.circle_radius, src.construction));
      continue;
    }
    const auto& pieces = p.pieces;
    const std::size_t n = pieces.size();
    const auto new_point = [&](dvec2 at) {
      const Id id = out.add_point(at);
      result.points.push_back(id);
      return id;
    };
    // Joint k is the start of piece k; the end of the last is joint n (or 0).
    std::vector<Id> joint(n + 1);
    joint[0] = new_point(pieces[0].start());
    for (std::size_t k = 1; k < n; ++k)
      joint[k] = new_point((pieces[k - 1].end() + pieces[k].start()) * 0.5);
    joint[n] = chain.closed ? joint[0] : new_point(pieces[n - 1].end());
    if (chain.closed) out.set_point(joint[0], (pieces[n - 1].end() + pieces[0].start()) * 0.5);

    // Corner arcs come right after the piece of the source curve before
    // their joint.
    std::size_t last_source = 0;
    for (std::size_t k = 0; k < n; ++k) {
      const Piece& pc = pieces[k];
      if (pc.source != kNone) last_source = pc.source;
      switch (pc.kind) {
        case Piece::Kind::Line:
          result.curves.push_back(out.add_line(joint[k], joint[k + 1], pc.construction));
          break;
        case Piece::Kind::Arc: {
          Id center = pc.center;
          if (pc.source == kNone) center = p.corner_points[last_source];
          if (center.is_null()) center = new_point(pc.c);
          result.curves.push_back(pc.sweep > 0.0
                                      ? out.add_arc(center, joint[k], joint[k + 1], pc.construction)
                                      : out.add_arc(center, joint[k + 1], joint[k], pc.construction));
          break;
        }
        case Piece::Kind::Bezier: {
          const Id h1 = new_point(pc.bz.p[1]);
          const Id h2 = new_point(pc.bz.p[2]);
          result.curves.push_back(out.add_bezier(joint[k], h1, h2, joint[k + 1], pc.construction));
          break;
        }
      }
    }
  }
  sketch = std::move(out);
  return result;
}

}  // namespace vs::sketch

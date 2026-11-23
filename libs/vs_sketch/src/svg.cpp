#include "vs/sketch/svg.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include "vs/core/xml.hpp"

namespace vs::sketch {
namespace {

constexpr double kPi = std::numbers::pi;

// --- the document as a tree ---------------------------------------------

struct Node {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<std::size_t> children;

  [[nodiscard]] std::optional<std::string_view> attribute(std::string_view key) const {
    for (const auto& [k, v] : attributes) {
      if (k == key) return std::string_view(v);
    }
    return std::nullopt;
  }
  // `href` or `xlink:href` (any prefix).
  [[nodiscard]] std::optional<std::string_view> href() const {
    for (const auto& [k, v] : attributes) {
      if (k == "href" || (k.size() > 5 && std::string_view(k).ends_with(":href"))) {
        return std::string_view(v);
      }
    }
    return std::nullopt;
  }
};

// Node 0 is a document node holding the outermost element.
std::optional<std::vector<Node>> parse_tree(std::string_view text) {
  std::vector<Node> nodes(1);
  std::vector<std::size_t> open{0};
  bool ok = true;
  const bool scanned = core::xml::scan_xml(text, [&](const core::xml::Tag& tag) {
    if (tag.closing) {
      if (open.size() < 2 || nodes[open.back()].name != tag.name) {
        ok = false;
        return false;
      }
      open.pop_back();
      return true;
    }
    Node n;
    n.name = std::string(tag.name);
    for (const auto& [k, v] : tag.attributes) n.attributes.emplace_back(std::string(k), v);
    const std::size_t index = nodes.size();
    nodes.push_back(std::move(n));
    nodes[open.back()].children.push_back(index);
    if (!tag.self_closing) open.push_back(index);
    return true;
  });
  if (!scanned || !ok || open.size() != 1) return std::nullopt;
  return nodes;
}

// --- numbers, lengths and transforms --------------------------------------

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ','; }

// Reads numbers as SVG writes them ("-1.5e3", ".5", "1.5.5" is 1.5 and .5).
class Scanner {
 public:
  explicit Scanner(std::string_view s) : s_(s) {}

  void skip_space() {
    while (i_ < s_.size() && is_space(s_[i_])) ++i_;
  }
  [[nodiscard]] bool done() {
    skip_space();
    return i_ >= s_.size();
  }
  [[nodiscard]] std::size_t position() const { return i_; }
  [[nodiscard]] char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
  void advance() { ++i_; }

  // True when a number starts here (after separators).
  [[nodiscard]] bool number_next() {
    skip_space();
    const char c = peek();
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
  }

  std::optional<double> number() {
    skip_space();
    const std::size_t start = i_;
    std::size_t p = i_;
    if (p < s_.size() && (s_[p] == '+' || s_[p] == '-')) ++p;
    const std::size_t digits = p;
    while (p < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p])) != 0) ++p;
    bool any = p > digits;
    if (p < s_.size() && s_[p] == '.') {
      ++p;
      const std::size_t frac = p;
      while (p < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p])) != 0) ++p;
      any = any || p > frac;
    }
    if (!any) return std::nullopt;
    if (p < s_.size() && (s_[p] == 'e' || s_[p] == 'E')) {
      std::size_t q = p + 1;
      if (q < s_.size() && (s_[q] == '+' || s_[q] == '-')) ++q;
      const std::size_t exp = q;
      while (q < s_.size() && std::isdigit(static_cast<unsigned char>(s_[q])) != 0) ++q;
      if (q > exp) p = q;
    }
    std::string_view text = s_.substr(start, p - start);
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    double v = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || end != text.data() + text.size() || !std::isfinite(v)) {
      return std::nullopt;
    }
    i_ = p;
    return v;
  }

  // An arc flag: a single 0 or 1, which may run into what follows.
  std::optional<bool> flag() {
    skip_space();
    if (peek() == '0' || peek() == '1') {
      const bool f = peek() == '1';
      ++i_;
      return f;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::string_view rest() const { return s_.substr(i_); }

 private:
  std::string_view s_;
  std::size_t i_ = 0;
};

std::string_view trim(std::string_view s) {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

// A length in user units (px), or nullopt for a missing, relative (%, em,
// ex) or unreadable one.
std::optional<double> length_px(std::optional<std::string_view> text, double ppi) {
  if (!text) return std::nullopt;
  Scanner s(trim(*text));
  const auto v = s.number();
  if (!v) return std::nullopt;
  const std::string_view unit = trim(s.rest());
  if (unit.empty() || unit == "px") return *v;
  if (unit == "in") return *v * ppi;
  if (unit == "mm") return *v * ppi / 25.4;
  if (unit == "cm") return *v * ppi / 2.54;
  if (unit == "pt") return *v * ppi / 72;
  if (unit == "pc") return *v * ppi / 6;
  return std::nullopt;
}

double length_or(const Node& n, std::string_view key, double ppi, double fallback) {
  return length_px(n.attribute(key), ppi).value_or(fallback);
}

// x' = a x + c y + e, y' = b x + d y + f (SVG's matrix(a b c d e f)).
struct Affine {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

  [[nodiscard]] dvec2 apply(dvec2 p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
  [[nodiscard]] dvec2 linear(dvec2 v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
  [[nodiscard]] double det() const { return a * d - b * c; }
  // This after `o`.
  [[nodiscard]] Affine operator*(const Affine& o) const {
    return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d,
            b * o.c + d * o.d, a * o.e + c * o.f + e, b * o.e + d * o.f + f};
  }
  static Affine translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
  static Affine scale(double x, double y) { return {x, 0, 0, y, 0, 0}; }
  static Affine rotate(double radians) {
    const double cs = std::cos(radians), sn = std::sin(radians);
    return {cs, sn, -sn, cs, 0, 0};
  }
};

// The `transform` attribute; nullopt when it can't be read.
std::optional<Affine> parse_transform(std::string_view text) {
  Affine m;
  std::size_t i = 0;
  while (true) {
    while (i < text.size() && is_space(text[i])) ++i;
    if (i >= text.size()) return m;
    const std::size_t open = text.find('(', i);
    if (open == std::string_view::npos) return std::nullopt;
    const std::size_t close = text.find(')', open);
    if (close == std::string_view::npos) return std::nullopt;
    const std::string_view name = trim(text.substr(i, open - i));
    std::vector<double> v;
    Scanner s(text.substr(open + 1, close - open - 1));
    while (!s.done()) {
      const auto x = s.number();
      if (!x) return std::nullopt;
      v.push_back(*x);
    }
    const double deg = kPi / 180;
    Affine t;
    if (name == "matrix" && v.size() == 6) {
      t = {v[0], v[1], v[2], v[3], v[4], v[5]};
    } else if (name == "translate" && (v.size() == 1 || v.size() == 2)) {
      t = Affine::translate(v[0], v.size() == 2 ? v[1] : 0);
    } else if (name == "scale" && (v.size() == 1 || v.size() == 2)) {
      t = Affine::scale(v[0], v.size() == 2 ? v[1] : v[0]);
    } else if (name == "rotate" && v.size() == 1) {
      t = Affine::rotate(v[0] * deg);
    } else if (name == "rotate" && v.size() == 3) {
      t = Affine::translate(v[1], v[2]) * Affine::rotate(v[0] * deg) *
          Affine::translate(-v[1], -v[2]);
    } else if (name == "skewX" && v.size() == 1) {
      t = {1, 0, std::tan(v[0] * deg), 1, 0, 0};
    } else if (name == "skewY" && v.size() == 1) {
      t = {1, std::tan(v[0] * deg), 0, 1, 0, 0};
    } else {
      return std::nullopt;
    }
    m = m * t;
    i = close + 1;
  }
}

struct ViewBox {
  double x = 0, y = 0, w = 0, h = 0;
};

std::optional<ViewBox> parse_view_box(std::optional<std::string_view> text) {
  if (!text) return std::nullopt;
  Scanner s(*text);
  ViewBox vb;
  for (double* v : {&vb.x, &vb.y, &vb.w, &vb.h}) {
    const auto n = s.number();
    if (!n) return std::nullopt;
    *v = *n;
  }
  if (!(vb.w > 0) || !(vb.h > 0)) return std::nullopt;
  return vb;
}

// The viewBox's map onto a viewport at (x, y) of size w by h, as
// preserveAspectRatio says (xMidYMid meet by default).
Affine view_box_map(const ViewBox& vb, double x, double y, double w, double h,
                    std::optional<std::string_view> aspect) {
  double sx = w / vb.w;
  double sy = h / vb.h;
  double ax = 0.5, ay = 0.5;
  std::string_view par = aspect ? trim(*aspect) : std::string_view{};
  if (par.starts_with("defer")) par = trim(par.substr(5));
  if (!par.starts_with("none")) {
    const bool slice = par.find("slice") != std::string_view::npos;
    if (par.size() >= 8 && par[0] == 'x') {
      const std::string_view xa = par.substr(1, 3), ya = par.substr(5, 3);
      ax = xa == "Min" ? 0 : xa == "Max" ? 1 : 0.5;
      ay = ya == "Min" ? 0 : ya == "Max" ? 1 : 0.5;
    }
    const double s = slice ? std::max(sx, sy) : std::min(sx, sy);
    sx = sy = s;
  } else {
    ax = ay = 0;
  }
  const double tx = x + (w - vb.w * sx) * ax - vb.x * sx;
  const double ty = y + (h - vb.h * sy) * ay - vb.y * sy;
  return {sx, 0, 0, sy, tx, ty};
}

// True when `style` (CSS declarations) or the attribute says display: none.
bool hidden(const Node& n) {
  if (const auto d = n.attribute("display"); d && trim(*d) == "none") return true;
  if (const auto style = n.attribute("style")) {
    std::string_view s = *style;
    while (!s.empty()) {
      const std::size_t semi = s.find(';');
      const std::string_view decl = s.substr(0, semi);
      const std::size_t colon = decl.find(':');
      if (colon != std::string_view::npos && trim(decl.substr(0, colon)) == "display" &&
          trim(decl.substr(colon + 1)) == "none") {
        return true;
      }
      if (semi == std::string_view::npos) break;
      s.remove_prefix(semi + 1);
    }
  }
  return false;
}

// --- geometry in sketch millimetres -----------------------------------------

struct Segment {
  enum class Kind { Line, Bezier, Arc } kind = Kind::Line;
  // Line: p[0] to p[1]. Bezier: p[0] to p[3]. Arc: p[0] to p[1] about
  // `center`, counter-clockwise when `ccw`.
  dvec2 p[4]{};
  dvec2 center{};
  bool ccw = true;

  [[nodiscard]] dvec2 start() const { return p[0]; }
  [[nodiscard]] dvec2 end() const { return kind == Kind::Bezier ? p[3] : p[1]; }
  // How far the segment reaches: its length, or more.
  [[nodiscard]] double extent() const {
    switch (kind) {
      case Kind::Line:
        return core::distance(p[0], p[1]);
      case Kind::Bezier:
        return core::distance(p[0], p[1]) + core::distance(p[1], p[2]) +
               core::distance(p[2], p[3]);
      case Kind::Arc: {
        const dvec2 a = p[0] - center, b = p[1] - center;
        double sweep = std::atan2(core::cross(a, b), core::dot(a, b));
        if (!ccw) sweep = -sweep;
        if (sweep < 0) sweep += 2 * kPi;
        return core::length(a) * sweep;
      }
    }
    return 0;
  }
};

struct CircleShape {
  dvec2 center;
  double radius = 0;
};

struct Output {
  std::vector<Segment> segments;
  std::vector<CircleShape> circles;
};

// Builds one element's path in user units, emitting segments in millimetres.
class PathBuilder {
 public:
  PathBuilder(const Affine& to_mm, double tolerance, Output& out, int& approximated)
      : t_(to_mm), tolerance_(tolerance), out_(out), approximated_(approximated) {}

  void move_to(dvec2 p) { pen_ = start_ = p; }
  void line_to(dvec2 p) {
    if (p != pen_) {
      Segment s;
      s.kind = Segment::Kind::Line;
      s.p[0] = t_.apply(pen_);
      s.p[1] = t_.apply(p);
      out_.segments.push_back(s);
    }
    pen_ = p;
  }
  void cubic_to(dvec2 c1, dvec2 c2, dvec2 p) {
    Segment s;
    s.kind = Segment::Kind::Bezier;
    s.p[0] = t_.apply(pen_);
    s.p[1] = t_.apply(c1);
    s.p[2] = t_.apply(c2);
    s.p[3] = t_.apply(p);
    out_.segments.push_back(s);
    pen_ = p;
  }
  void close() {
    line_to(start_);
    pen_ = start_;
  }
  [[nodiscard]] dvec2 pen() const { return pen_; }

  // SVG's endpoint arc (implementation notes F.6.5), from the pen to `p`.
  void arc_to(double rx, double ry, double phi_degrees, bool large, bool sweep, dvec2 p) {
    const dvec2 p1 = pen_;
    if (p1 == p) return;
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx == 0 || ry == 0) {
      line_to(p);
      return;
    }
    const double phi = phi_degrees * kPi / 180;
    const double cs = std::cos(phi), sn = std::sin(phi);
    const dvec2 h = (p1 - p) * 0.5;
    const double x1 = cs * h.x + sn * h.y;
    const double y1 = -sn * h.x + cs * h.y;
    const double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1) {
      rx *= std::sqrt(lambda);
      ry *= std::sqrt(lambda);
    }
    const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
    if (large == sweep) coef = -coef;
    const double cxp = coef * rx * y1 / ry;
    const double cyp = -coef * ry * x1 / rx;
    const dvec2 mid = (p1 + p) * 0.5;
    const dvec2 c{cs * cxp - sn * cyp + mid.x, sn * cxp + cs * cyp + mid.y};
    const dvec2 u{(x1 - cxp) / rx, (y1 - cyp) / ry};
    const dvec2 v{(-x1 - cxp) / rx, (-y1 - cyp) / ry};
    const double theta1 = std::atan2(u.y, u.x);
    double dtheta = std::atan2(core::cross(u, v), core::dot(u, v));
    if (!sweep && dtheta > 0) dtheta -= 2 * kPi;
    if (sweep && dtheta < 0) dtheta += 2 * kPi;
    elliptic(c, rx, ry, phi, theta1, dtheta, p);
  }

  // The ellipse arc c + R(phi) (rx cos θ, ry sin θ) from θ1 through dθ,
  // from the pen to `end`.
  void elliptic(dvec2 c, double rx, double ry, double phi, double theta1, double dtheta,
                dvec2 end) {
    const Affine e = Affine::translate(c.x, c.y) * Affine::rotate(phi) * Affine::scale(rx, ry);
    const Affine m = t_ * e;  // the unit circle into millimetres
    const dvec2 m1{m.a, m.b}, m2{m.c, m.d};
    const double l1 = core::length(m1), l2 = core::length(m2);
    const double big = std::max(l1, l2);
    const bool circular = std::abs(l1 - l2) <= 1e-9 * big &&
                          std::abs(core::dot(m1, m2)) <= 1e-9 * l1 * l2;
    if (circular) {
      Segment s;
      s.kind = Segment::Kind::Arc;
      s.p[0] = t_.apply(pen_);
      s.p[1] = t_.apply(end);
      s.center = m.apply({0, 0});
      s.ccw = (dtheta > 0) == (m.det() > 0);
      out_.segments.push_back(s);
      pen_ = end;
      return;
    }
    // Cubics on the unit circle, mapped: a quarter turn misses by 2.7e-4 of
    // the radius, and the miss grows as the sixth power of the angle.
    ++approximated_;
    const double quarter = kPi / 2;
    double step = quarter * std::pow(tolerance_ / (2.7e-4 * big), 1.0 / 6.0);
    step = std::min(step, quarter);
    const int n = std::max(1, static_cast<int>(std::ceil(std::abs(dtheta) / step - 1e-9)));
    const double d = dtheta / n;
    const double k = 4.0 / 3.0 * std::tan(d / 4);
    for (int i = 0; i < n; ++i) {
      const double a = theta1 + d * i, b = theta1 + d * (i + 1);
      const dvec2 pa{std::cos(a), std::sin(a)}, pb{std::cos(b), std::sin(b)};
      const dvec2 c1 = pa + dvec2{-pa.y, pa.x} * k;
      const dvec2 c2 = pb - dvec2{-pb.y, pb.x} * k;
      Segment s;
      s.kind = Segment::Kind::Bezier;
      s.p[0] = t_.apply(pen_);
      s.p[1] = m.apply(c1);
      s.p[2] = m.apply(c2);
      const dvec2 to = i + 1 == n ? end : e.apply(pb);
      s.p[3] = t_.apply(to);
      out_.segments.push_back(s);
      pen_ = to;
    }
  }

 private:
  Affine t_;
  double tolerance_;
  Output& out_;
  int& approximated_;
  dvec2 pen_{}, start_{};
};

// Path data. Returns the position of the first character it couldn't read,
// or nullopt when it read everything.
std::optional<std::size_t> parse_path(std::string_view d, PathBuilder& b) {
  Scanner s(d);
  char command = '\0';
  char previous = '\0';
  dvec2 last_control{};  // the previous C/S's second handle, or Q/T's control
  bool started = false;
  while (!s.done()) {
    const std::size_t at = s.position();
    const char c = s.peek();
    if (std::isalpha(static_cast<unsigned char>(c)) != 0) {
      command = c;
      s.advance();
      if (command == 'Z' || command == 'z') {
        if (!started) return at;
        b.close();
        previous = command;
        continue;
      }
    } else if (command == '\0' || command == 'Z' || command == 'z') {
      return at;
    }
    if (!started && command != 'M' && command != 'm') return at;
    const bool rel = std::islower(static_cast<unsigned char>(command)) != 0;
    const dvec2 o = rel ? b.pen() : dvec2{0, 0};
    const auto num = [&]() { return s.number(); };
    const auto pair = [&]() -> std::optional<dvec2> {
      const auto x = num();
      if (!x) return std::nullopt;
      const auto y = num();
      if (!y) return std::nullopt;
      return dvec2{*x, *y};
    };
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
    switch (upper) {
      case 'M': {
        const auto p = pair();
        if (!p) return at;
        b.move_to(o + *p);
        started = true;
        command = rel ? 'l' : 'L';  // further pairs are line-tos
        break;
      }
      case 'L': {
        const auto p = pair();
        if (!p) return at;
        b.line_to(o + *p);
        break;
      }
      case 'H': {
        const auto x = num();
        if (!x) return at;
        b.line_to({(rel ? b.pen().x : 0) + *x, b.pen().y});
        break;
      }
      case 'V': {
        const auto y = num();
        if (!y) return at;
        b.line_to({b.pen().x, (rel ? b.pen().y : 0) + *y});
        break;
      }
      case 'C': {
        const auto c1 = pair();
        const auto c2 = c1 ? pair() : std::nullopt;
        const auto p = c2 ? pair() : std::nullopt;
        if (!p) return at;
        last_control = o + *c2;
        b.cubic_to(o + *c1, last_control, o + *p);
        break;
      }
      case 'S': {
        const auto c2 = pair();
        const auto p = c2 ? pair() : std::nullopt;
        if (!p) return at;
        const char pu = static_cast<char>(std::toupper(static_cast<unsigned char>(previous)));
        const dvec2 c1 = (pu == 'C' || pu == 'S') ? b.pen() * 2.0 - last_control : b.pen();
        last_control = o + *c2;
        b.cubic_to(c1, last_control, o + *p);
        break;
      }
      case 'Q':
      case 'T': {
        dvec2 q;
        if (upper == 'Q') {
          const auto qc = pair();
          if (!qc) return at;
          q = o + *qc;
        } else {
          const char pu = static_cast<char>(std::toupper(static_cast<unsigned char>(previous)));
          q = (pu == 'Q' || pu == 'T') ? b.pen() * 2.0 - last_control : b.pen();
        }
        const auto p = pair();
        if (!p) return at;
        const dvec2 p0 = b.pen(), p3 = o + *p;
        last_control = q;
        // Raised exactly: handles two thirds of the way to the control point.
        b.cubic_to(p0 + (q - p0) * (2.0 / 3.0), p3 + (q - p3) * (2.0 / 3.0), p3);
        break;
      }
      case 'A': {
        const auto rx = num();
        const auto ry = rx ? num() : std::nullopt;
        const auto phi = ry ? num() : std::nullopt;
        const auto large = phi ? s.flag() : std::nullopt;
        const auto sweep = large ? s.flag() : std::nullopt;
        const auto p = sweep ? pair() : std::nullopt;
        if (!p) return at;
        b.arc_to(*rx, *ry, *phi, *large, *sweep, o + *p);
        break;
      }
      default:
        return at;
    }
    previous = command;
  }
  return std::nullopt;
}

// --- walking the document -----------------------------------------------------

class Walker {
 public:
  Walker(const std::vector<Node>& nodes, const SvgImportOptions& options,
         const core::ProductSettings& settings)
      : nodes_(nodes), options_(options), settings_(settings) {
    for (std::size_t i = 1; i < nodes.size(); ++i) {
      if (const auto id = nodes[i].attribute("id")) ids_.emplace(std::string(*id), i);
    }
  }

  void root(std::size_t svg) {
    const Node& n = nodes_[svg];
    const double ppi = options_.pixels_per_inch;
    const double mm_per_px = 25.4 / ppi;
    const auto vb = parse_view_box(n.attribute("viewBox"));
    std::optional<double> w = length_px(n.attribute("width"), ppi);
    std::optional<double> h = length_px(n.attribute("height"), ppi);
    if (vb) {
      if (!w && h) w = *h * vb->w / vb->h;
      if (!h && w) h = *w * vb->h / vb->w;
      if (!w) w = vb->w;
      if (!h) h = vb->h;
    }
    Affine to_mm = Affine::scale(mm_per_px, mm_per_px);
    if (vb) to_mm = to_mm * view_box_map(*vb, 0, 0, *w, *h, n.attribute("preserveAspectRatio"));
    // Y up, the viewport's lower left corner at the origin.
    const double height_mm = h ? *h * mm_per_px : 0;
    const Affine flip{1, 0, 0, -1, 0, height_mm};
    const Affine total = Affine::scale(options_.scale, options_.scale) * flip * to_mm;
    children(svg, with_transform(n, total));
  }

  Output output;
  Diagnostics diagnostics;
  int approximated = 0;
  std::map<std::string, int> unsupported;

 private:
  Affine with_transform(const Node& n, const Affine& t) {
    const auto text = n.attribute("transform");
    if (!text) return t;
    const auto m = parse_transform(*text);
    if (!m) {
      diagnostics.add(Diagnostic::warning(
          "sketch.import.transform",
          fmt::format("The transform of {} can't be read and was left out: \"{}\"", label(n),
                      *text)));
      return t;
    }
    return t * *m;
  }

  [[nodiscard]] std::string label(const Node& n) const {
    if (const auto id = n.attribute("id")) return fmt::format("<{} id=\"{}\">", n.name, *id);
    return fmt::format("a <{}>", n.name);
  }

  void children(std::size_t index, const Affine& t) {
    for (const std::size_t child : nodes_[index].children) element(child, t);
  }

  void element(std::size_t index, const Affine& parent) {
    const Node& n = nodes_[index];
    if (hidden(n)) return;
    const std::string& k = n.name;
    const double ppi = options_.pixels_per_inch;
    const auto len = [&](std::string_view key, double fallback = 0) {
      return length_or(n, key, ppi, fallback);
    };
    if (k == "g" || k == "a" || k == "switch") {
      children(index, with_transform(n, parent));
    } else if (k == "svg") {
      Affine t = with_transform(n, parent) * Affine::translate(len("x"), len("y"));
      if (const auto vb = parse_view_box(n.attribute("viewBox"))) {
        t = t * view_box_map(*vb, 0, 0, len("width", vb->w), len("height", vb->h),
                             n.attribute("preserveAspectRatio"));
      }
      children(index, t);
    } else if (k == "use") {
      use(index, parent);
    } else if (k == "path") {
      const auto d = n.attribute("d");
      if (!d) return;
      PathBuilder b(with_transform(n, parent), tolerance(), output, approximated);
      if (const auto stop = parse_path(*d, b)) {
        diagnostics.add(Diagnostic::warning(
            "sketch.import.path_data",
            fmt::format("The path data of {} can't be read from character {} on; the path up "
                        "to there came in",
                        label(n), *stop)));
      }
    } else if (k == "rect") {
      rect(n, with_transform(n, parent));
    } else if (k == "circle" || k == "ellipse") {
      const double r = len("r");
      const double rx = k == "circle" ? r : len("rx");
      const double ry = k == "circle" ? r : len("ry");
      ellipse(dvec2{len("cx"), len("cy")}, rx, ry, with_transform(n, parent));
    } else if (k == "line") {
      PathBuilder b(with_transform(n, parent), tolerance(), output, approximated);
      b.move_to({len("x1"), len("y1")});
      b.line_to({len("x2"), len("y2")});
    } else if (k == "polyline" || k == "polygon") {
      PathBuilder b(with_transform(n, parent), tolerance(), output, approximated);
      Scanner s(n.attribute("points").value_or(""));
      bool first = true;
      while (!s.done()) {
        const auto x = s.number();
        const auto y = x ? s.number() : std::nullopt;
        if (!y) break;
        if (first) {
          b.move_to({*x, *y});
        } else {
          b.line_to({*x, *y});
        }
        first = false;
      }
      if (!first && k == "polygon") b.close();
    } else if (k == "text" || k == "image" || k == "foreignObject" || k == "video") {
      ++unsupported[k];
    }
    // Anything else (defs, symbol, clipPath, mask, pattern, marker, style,
    // metadata, gradients, editor data, ...) draws nothing by itself.
  }

  void use(std::size_t index, const Affine& parent) {
    const Node& n = nodes_[index];
    const double ppi = options_.pixels_per_inch;
    const auto href = n.href();
    const auto it = href && href->starts_with('#') ? ids_.find(std::string(href->substr(1)))
                                                     : ids_.end();
    if (it == ids_.end()) {
      diagnostics.add(Diagnostic::warning(
          "sketch.import.reference",
          fmt::format("{} refers to \"{}\", which isn't in the file", label(n),
                      href.value_or(""))));
      return;
    }
    if (active_.contains(it->second) || active_.size() > 64) {
      diagnostics.add(Diagnostic::warning(
          "sketch.import.reference",
          fmt::format("{} uses itself through \"{}\" and was left out", label(n), *href)));
      return;
    }
    Affine t = with_transform(n, parent) *
               Affine::translate(length_or(n, "x", ppi, 0), length_or(n, "y", ppi, 0));
    const Node& target = nodes_[it->second];
    active_.insert(it->second);
    if (target.name == "symbol") {
      if (hidden(target)) {
        active_.erase(it->second);
        return;
      }
      if (const auto vb = parse_view_box(target.attribute("viewBox"))) {
        t = t * view_box_map(*vb, 0, 0, length_or(n, "width", ppi, vb->w),
                             length_or(n, "height", ppi, vb->h),
                             target.attribute("preserveAspectRatio"));
      }
      children(it->second, with_transform(target, t));
    } else {
      element(it->second, t);
    }
    active_.erase(it->second);
  }

  void rect(const Node& n, const Affine& t) {
    const double ppi = options_.pixels_per_inch;
    const double x = length_or(n, "x", ppi, 0), y = length_or(n, "y", ppi, 0);
    const double w = length_or(n, "width", ppi, 0), h = length_or(n, "height", ppi, 0);
    if (!(w > 0) || !(h > 0)) return;
    auto rx = length_px(n.attribute("rx"), ppi);
    auto ry = length_px(n.attribute("ry"), ppi);
    if (rx && *rx < 0) rx.reset();
    if (ry && *ry < 0) ry.reset();
    if (!rx) rx = ry;
    if (!ry) ry = rx;
    const double a = std::min(rx.value_or(0), w / 2);
    const double b = std::min(ry.value_or(0), h / 2);
    PathBuilder p(t, tolerance(), output, approximated);
    if (a > 0 && b > 0) {
      p.move_to({x + a, y});
      p.line_to({x + w - a, y});
      p.arc_to(a, b, 0, false, true, {x + w, y + b});
      p.line_to({x + w, y + h - b});
      p.arc_to(a, b, 0, false, true, {x + w - a, y + h});
      p.line_to({x + a, y + h});
      p.arc_to(a, b, 0, false, true, {x, y + h - b});
      p.line_to({x, y + b});
      p.arc_to(a, b, 0, false, true, {x + a, y});
    } else {
      p.move_to({x, y});
      p.line_to({x + w, y});
      p.line_to({x + w, y + h});
      p.line_to({x, y + h});
      p.close();
    }
  }

  void ellipse(dvec2 c, double rx, double ry, const Affine& t) {
    if (!(rx > 0) || !(ry > 0)) return;
    const dvec2 m1 = t.linear({rx, 0}), m2 = t.linear({0, ry});
    const double l1 = core::length(m1), l2 = core::length(m2);
    if (std::abs(l1 - l2) <= 1e-9 * std::max(l1, l2) &&
        std::abs(core::dot(m1, m2)) <= 1e-9 * l1 * l2) {
      output.circles.push_back({t.apply(c), (l1 + l2) / 2});
      return;
    }
    PathBuilder p(t, tolerance(), output, approximated);
    p.move_to(c + dvec2{rx, 0});
    p.elliptic(c, rx, ry, 0, 0, kPi, c - dvec2{rx, 0});
    p.elliptic(c, rx, ry, 0, kPi, kPi, c + dvec2{rx, 0});
  }

  [[nodiscard]] double tolerance() const { return settings_.min_feature_size_mm(); }

  const std::vector<Node>& nodes_;
  const SvgImportOptions& options_;
  const core::ProductSettings& settings_;
  std::map<std::string, std::size_t> ids_;
  std::set<std::size_t> active_;
};

// --- joining ends and adding to the sketch --------------------------------------

struct UnionFind {
  explicit UnionFind(std::size_t n) : parent(n) {
    for (std::size_t i = 0; i < n; ++i) parent[i] = i;
  }
  std::size_t find(std::size_t i) {
    while (parent[i] != i) i = parent[i] = parent[parent[i]];
    return i;
  }
  void unite(std::size_t a, std::size_t b) {
    a = find(a);
    b = find(b);
    if (a == b) return;
    if (b < a) std::swap(a, b);
    parent[b] = a;  // the smallest index represents its set
  }
  std::vector<std::size_t> parent;
};

// The direction from the curve's end at point `at` into the curve.
dvec2 direction_into(const Sketch& sk, const Entity& e, const Id& at) {
  if (const auto* l = e.as<Line>()) {
    const dvec2 a = *sk.point(l->start), b = *sk.point(l->end);
    return core::normalize(l->start == at ? b - a : a - b);
  }
  if (const auto* a = e.as<Arc>()) {
    const dvec2 c = *sk.point(a->center);
    const dvec2 p = *sk.point(at);
    const dvec2 travel = core::normalize(dvec2{-(p.y - c.y), p.x - c.x});
    return a->start == at ? travel : -travel;
  }
  if (const auto* b = e.as<Bezier>()) {
    const auto g = bezier_geometry(sk, e.id);
    return b->start == at ? g->tangent(false) : -g->tangent(true);
  }
  return {};
}

// A Bézier's handle at `at` is long enough to carry a tangent.
bool handle_ok(const Sketch& sk, const Entity& e, const Id& at, double gap) {
  const auto* b = e.as<Bezier>();
  if (b == nullptr) return true;
  const dvec2 h = *sk.point(b->start == at ? b->control1 : b->control2);
  return core::distance(*sk.point(at), h) > gap;
}

Result<SvgImport> add_to_sketch(Output out, Sketch& sketch, Diagnostics diagnostics,
                                const SvgImportOptions& options) {
  const double gap = options.gap_mm;
  // Segments shorter than the gap are dropped; their ends still join.
  std::vector<Segment> segs;
  for (const Segment& s : out.segments) {
    if (s.extent() > gap) segs.push_back(s);
  }
  std::vector<CircleShape> circles;
  for (const CircleShape& c : out.circles) {
    if (c.radius * 2 * kPi > gap) circles.push_back(c);
  }
  if (segs.empty() && circles.empty()) {
    return Diagnostic::error("sketch.import.empty", "The file has no paths or shapes to import");
  }

  // Join ends within the gap: a sweep along x.
  const std::size_t ends = segs.size() * 2;
  const auto end_at = [&](std::size_t i) {
    return i % 2 == 0 ? segs[i / 2].start() : segs[i / 2].end();
  };
  std::vector<std::size_t> order(ends);
  for (std::size_t i = 0; i < ends; ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) { return end_at(a).x < end_at(b).x; });
  UnionFind joined(ends);
  for (std::size_t i = 0; i < ends; ++i) {
    const dvec2 p = end_at(order[i]);
    for (std::size_t j = i + 1; j < ends && end_at(order[j]).x - p.x <= gap; ++j) {
      if (core::distance(p, end_at(order[j])) <= gap) joined.unite(order[i], order[j]);
    }
  }
  const auto cluster = [&](std::size_t seg, int end) {
    return joined.find(seg * 2 + static_cast<std::size_t>(end));
  };

  // Curves not on a loop: peel off curves with a free end until none is
  // left. A line or arc whose ends joined into one point is dropped.
  std::vector<bool> alive(segs.size(), true);
  for (std::size_t s = 0; s < segs.size(); ++s) {
    if (segs[s].kind != Segment::Kind::Bezier && cluster(s, 0) == cluster(s, 1)) {
      alive[s] = false;
    }
  }
  std::vector<bool> kept = alive;  // dropped degenerate curves never reach the sketch
  std::map<std::size_t, std::vector<std::size_t>> at_point;
  std::map<std::size_t, int> degree;
  for (std::size_t s = 0; s < segs.size(); ++s) {
    if (!alive[s]) continue;
    for (int e = 0; e < 2; ++e) {
      at_point[cluster(s, e)].push_back(s);
      ++degree[cluster(s, e)];
    }
  }
  std::vector<std::size_t> free_points;
  for (const auto& [p, d] : degree) {
    if (d == 1) free_points.push_back(p);
  }
  std::vector<bool> open(segs.size(), false);
  while (!free_points.empty()) {
    const std::size_t p = free_points.back();
    free_points.pop_back();
    if (degree[p] != 1) continue;
    for (const std::size_t s : at_point[p]) {
      if (!alive[s]) continue;
      alive[s] = false;
      open[s] = true;
      for (int e = 0; e < 2; ++e) {
        const std::size_t q = cluster(s, e);
        if (--degree[q] == 1) free_points.push_back(q);
      }
      break;
    }
  }

  // Arcs meeting end to end on one circle share their centre point. A loop
  // of them all the way round is a circle.
  const auto radius_of = [&](std::size_t s) { return core::distance(segs[s].p[0], segs[s].center); };
  UnionFind same_circle(segs.size());
  for (const auto& [p, curves] : at_point) {
    for (std::size_t i = 0; i < curves.size(); ++i) {
      for (std::size_t j = i + 1; j < curves.size(); ++j) {
        const std::size_t a = curves[i], b = curves[j];
        if (segs[a].kind == Segment::Kind::Arc && segs[b].kind == Segment::Kind::Arc &&
            core::distance(segs[a].center, segs[b].center) <= gap &&
            std::abs(radius_of(a) - radius_of(b)) <= gap) {
          same_circle.unite(a, b);
        }
      }
    }
  }
  {
    std::map<std::size_t, std::vector<std::size_t>> groups;
    for (std::size_t s = 0; s < segs.size(); ++s) {
      if (kept[s] && segs[s].kind == Segment::Kind::Arc) groups[same_circle.find(s)].push_back(s);
    }
    for (const auto& [root, members] : groups) {
      if (members.size() < 2) continue;
      // Every end meets exactly one other arc of the group and nothing else,
      // and the arcs turn once round.
      std::map<std::size_t, int> ends_here;
      double turn = 0;
      bool loop = true;
      for (const std::size_t s : members) {
        loop = loop && !open[s];
        for (int e = 0; e < 2; ++e) ++ends_here[cluster(s, e)];
        turn += segs[s].extent() / radius_of(s);
      }
      for (const auto& [p, n] : ends_here) loop = loop && n == 2 && degree[p] == 2;
      if (!loop || std::abs(turn - 2 * kPi) > 1e-6) continue;
      for (const std::size_t s : members) kept[s] = false;
      circles.push_back({segs[root].center, radius_of(root)});
    }
  }

  // Into a copy of the sketch.
  Sketch sk = sketch;
  SvgImport result;
  result.diagnostics = std::move(diagnostics);
  std::map<std::size_t, Id> points;
  std::vector<Id> point_order;  // curve ends and handles, for Fix
  const auto point_for = [&](std::size_t c, bool construction) {
    const auto it = points.find(c);
    if (it != points.end()) {
      // A point shared with a curve on a loop isn't construction.
      if (!construction) sk.find_entity(it->second)->construction = false;
      return it->second;
    }
    const Id id = sk.add_point(end_at(c), construction);
    points.emplace(c, id);
    point_order.push_back(id);
    return id;
  };
  std::vector<Id> entity_of(segs.size());
  std::vector<Id> arcs;
  std::map<std::size_t, Id> centers;   // by same_circle group
  std::map<Id, int> arcs_at_center;
  for (std::size_t s = 0; s < segs.size(); ++s) {
    if (!kept[s]) continue;
    const Segment& g = segs[s];
    const bool construction = open[s];
    const Id a = point_for(cluster(s, 0), construction);
    const Id b = point_for(cluster(s, 1), construction);
    Id id;
    switch (g.kind) {
      case Segment::Kind::Line:
        id = sk.add_line(a, b, construction);
        break;
      case Segment::Kind::Bezier: {
        const Id c1 = sk.add_point(g.p[1], construction);
        const Id c2 = sk.add_point(g.p[2], construction);
        point_order.push_back(c1);
        point_order.push_back(c2);
        id = sk.add_bezier(a, c1, c2, b, construction);
        break;
      }
      case Segment::Kind::Arc: {
        const std::size_t group = same_circle.find(s);
        auto it = centers.find(group);
        if (it == centers.end()) {
          it = centers.emplace(group, sk.add_point(segs[group].center, construction)).first;
        } else if (!construction) {
          sk.find_entity(it->second)->construction = false;
        }
        const Id c = it->second;
        ++arcs_at_center[c];
        id = g.ccw ? sk.add_arc(c, a, b, construction) : sk.add_arc(c, b, a, construction);
        arcs.push_back(id);
        break;
      }
    }
    entity_of[s] = id;
    result.curves.push_back(id);
    if (construction) result.open.push_back(id);
  }
  std::vector<Id> circle_ids;
  for (const CircleShape& c : circles) {
    const Id id = sk.add_circle(c.center, c.radius);
    circle_ids.push_back(id);
    result.curves.push_back(id);
  }

  // One warning per open piece (curves joined end to end).
  {
    UnionFind pieces(segs.size());
    std::map<std::size_t, std::size_t> first_at;
    for (std::size_t s = 0; s < segs.size(); ++s) {
      if (!open[s]) continue;
      for (int e = 0; e < 2; ++e) {
        const auto [it, added] = first_at.emplace(cluster(s, e), s);
        if (!added) pieces.unite(it->second, s);
      }
    }
    std::map<std::size_t, std::vector<std::size_t>> by_piece;
    for (std::size_t s = 0; s < segs.size(); ++s) {
      if (open[s]) by_piece[pieces.find(s)].push_back(s);
    }
    for (const auto& [root, members] : by_piece) {
      std::vector<core::SubjectRef> subjects;
      for (const std::size_t s : members) subjects.push_back(sk.subject(entity_of[s]));
      const dvec2 at = segs[members.front()].start();
      result.diagnostics.add(Diagnostic::warning(
          "sketch.import.open",
          fmt::format("An open path ({} {}, starting near ({:.3f}, {:.3f}) mm) doesn't close "
                      "within {} mm and came in as construction geometry",
                      members.size(), members.size() == 1 ? "curve" : "curves", at.x, at.y,
                      gap),
          std::move(subjects)));
    }
  }

  if (options.fix) {
    for (const Id& p : point_order) result.constraints.push_back(sk.fix(p));
    for (const Id& c : circle_ids) result.constraints.push_back(sk.fix(c));
    // With its ends fixed, an arc's centre can only move along the chord's
    // perpendicular bisector: hold it along whichever axis that line leans to
    // (a radius would do it too, but not for a half circle, where the radius
    // is at its least and the solver finds it redundant).
    // Arcs sharing a centre fix it between them through their ends.
    for (const Id& a : arcs) {
      const Arc& arc = *sk.find_entity(a)->as<Arc>();
      if (arcs_at_center[arc.center] > 1) continue;
      const dvec2 s0 = *sk.point(arc.start), c = *sk.point(arc.center);
      const dvec2 chord = *sk.point(arc.end) - s0;
      const bool along_x = std::abs(chord.y) >= std::abs(chord.x);
      result.constraints.push_back(
          along_x ? sk.add_constraint(ConstraintKind::HorizontalDistance, {arc.start, arc.center},
                                      c.x - s0.x)
                  : sk.add_constraint(ConstraintKind::VerticalDistance, {arc.start, arc.center},
                                      c.y - s0.y));
    }
  } else if (options.smooth_joints) {
    const double cos_limit = std::cos(kSmoothJointAngle);
    std::set<std::pair<Id, Id>> tangent_pairs;
    for (const auto& [p, curves] : at_point) {
      std::vector<std::pair<std::size_t, int>> here;  // (segment, end) on loops
      for (const std::size_t s : curves) {
        if (open[s] || !kept[s]) continue;
        for (int e = 0; e < 2; ++e) {
          if (cluster(s, e) == p) here.emplace_back(s, e);
        }
      }
      if (here.size() != 2 || here[0].first == here[1].first) continue;
      const Entity& ea = *sk.find_entity(entity_of[here[0].first]);
      const Entity& eb = *sk.find_entity(entity_of[here[1].first]);
      if (ea.kind() == EntityKind::Line && eb.kind() == EntityKind::Line) continue;
      // Arcs sharing a centre are already smooth.
      if (ea.kind() == EntityKind::Arc && eb.kind() == EntityKind::Arc &&
          ea.as<Arc>()->center == eb.as<Arc>()->center) {
        continue;
      }
      const Id& at = points.at(p);
      if (!handle_ok(sk, ea, at, gap) || !handle_ok(sk, eb, at, gap)) continue;
      const dvec2 da = direction_into(sk, ea, at);
      const dvec2 db = direction_into(sk, eb, at);
      if (core::dot(da, db) > -cos_limit) continue;
      const auto key = std::minmax(ea.id, eb.id);
      if (!tangent_pairs.insert({key.first, key.second}).second) continue;
      // A line goes first (the form is (line, arc)).
      const bool swap = eb.kind() == EntityKind::Line;
      result.constraints.push_back(swap ? sk.tangent(eb.id, ea.id) : sk.tangent(ea.id, eb.id));
    }
  }

  sketch = std::move(sk);
  return result;
}

}  // namespace

Result<SvgImport> import_svg(std::string_view svg, Sketch& sketch, const SvgImportOptions& options,
                             const core::ProductSettings& settings) {
  const auto positive = [](double v) { return std::isfinite(v) && v > 0; };
  if (!positive(options.pixels_per_inch) || !positive(options.scale) ||
      !std::isfinite(options.gap_mm) || options.gap_mm < 0) {
    return Diagnostic::error("sketch.import.option",
                             "Pixels per inch and scale must be positive, and the gap at least 0");
  }
  const auto nodes = parse_tree(svg);
  if (!nodes || (*nodes)[0].children.empty() || (*nodes)[(*nodes)[0].children.front()].name != "svg") {
    return Diagnostic::error("sketch.import.malformed", "The file isn't a readable SVG document");
  }
  Walker walker(*nodes, options, settings);
  walker.root((*nodes)[0].children.front());
  Diagnostics diagnostics = std::move(walker.diagnostics);
  for (const auto& [kind, count] : walker.unsupported) {
    diagnostics.add(Diagnostic::warning(
        "sketch.import.unsupported",
        fmt::format("{} <{}> {} skipped: only paths and shapes are imported", count, kind,
                    count == 1 ? "element was" : "elements were")));
  }
  if (walker.approximated > 0) {
    diagnostics.add(Diagnostic::warning(
        "sketch.import.ellipse_approximated",
        fmt::format("{} elliptical {} approximated by Bézier curves within {} mm",
                    walker.approximated, walker.approximated == 1 ? "arc was" : "arcs were",
                    settings.min_feature_size_mm())));
  }
  return add_to_sketch(std::move(walker.output), sketch, std::move(diagnostics), options);
}

Result<SvgImport> import_svg_file(const std::filesystem::path& path, Sketch& sketch,
                                  const SvgImportOptions& options,
                                  const core::ProductSettings& settings) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Diagnostic::error("sketch.import.file",
                             fmt::format("Can't read \"{}\"", path.string()));
  }
  std::ostringstream text;
  text << in.rdbuf();
  return import_svg(text.str(), sketch, options, settings);
}

}  // namespace vs::sketch

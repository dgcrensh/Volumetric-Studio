#include "vs/mesh/triangulate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace vs::mesh {
namespace {

using core::dvec2;
using core::Result;

constexpr std::uint32_t kNone = 0xffffffffu;

Diagnostic error(std::string code, std::string message) {
  return Diagnostic::error(std::move(code), std::move(message));
}

bool before(const dvec2& p, const dvec2& q) { return p.x < q.x || (p.x == q.x && p.y < q.y); }

// Twice the signed area of (a, b, c). It is computed from the three points
// in one canonical order (and the sign of the permutation applied), so it
// is exactly antisymmetric: orient(a, b, c) == -orient(b, a, c) bit for
// bit, and every test that sees the same three points agrees on their
// side, whichever corner it measures from (H4).
double orient(const dvec2& a, const dvec2& b, const dvec2& c) {
  const dvec2* p = &a;
  const dvec2* q = &b;
  const dvec2* r = &c;
  bool odd = false;
  if (before(*q, *p)) {
    std::swap(p, q);
    odd = !odd;
  }
  if (before(*r, *q)) {
    std::swap(q, r);
    odd = !odd;
  }
  if (before(*q, *p)) {
    std::swap(p, q);
    odd = !odd;
  }
  const double o = (q->x - p->x) * (r->y - p->y) - (q->y - p->y) * (r->x - p->x);
  return odd ? -o : o;
}

// A threshold under which orient(a, b, c) counts as zero: rounding in the
// products is about 1e-16 of their size. Symmetric in the three points
// (the product of the two longest sides), so it is the same threshold
// whichever order a caller names them in.
double orient_eps(const dvec2& a, const dvec2& b, const dvec2& c) {
  const double ab = std::abs(b.x - a.x) + std::abs(b.y - a.y);
  const double bc = std::abs(c.x - b.x) + std::abs(c.y - b.y);
  const double ca = std::abs(a.x - c.x) + std::abs(a.y - c.y);
  return 1e-12 * std::max({ab * bc, bc * ca, ca * ab});
}

// Positive when d lies inside the circle through a, b, c (counter-clockwise).
// Returns 0 within rounding, so near-cocircular points never cycle flips.
double incircle(const dvec2& a, const dvec2& b, const dvec2& c, const dvec2& d) {
  const double adx = a.x - d.x, ady = a.y - d.y;
  const double bdx = b.x - d.x, bdy = b.y - d.y;
  const double cdx = c.x - d.x, cdy = c.y - d.y;
  const double ad = adx * adx + ady * ady;
  const double bd = bdx * bdx + bdy * bdy;
  const double cd = cdx * cdx + cdy * cdy;
  const double det = adx * (bdy * cd - bd * cdy) - ady * (bdx * cd - bd * cdx) +
                     ad * (bdx * cdy - bdy * cdx);
  const double scale = std::max({ad, bd, cd});
  return std::abs(det) <= 1e-12 * scale * scale ? 0.0 : det;
}

std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
  if (a > b) std::swap(a, b);
  return (static_cast<std::uint64_t>(a) << 32) | b;
}

// Edge k of a triangle is the one opposite corner k: (v[k+1], v[k+2]).
struct Tri {
  std::array<std::uint32_t, 3> v{};
  std::array<std::uint32_t, 3> n{kNone, kNone, kNone};
};

std::uint32_t inc(std::uint32_t k) { return k == 2 ? 0 : k + 1; }
std::uint32_t dec(std::uint32_t k) { return k == 0 ? 2 : k - 1; }

class Cdt {
 public:
  explicit Cdt(std::vector<dvec2> points) : p_(std::move(points)) {}

  // Builds the Delaunay triangulation of every point inside a super
  // triangle. False on numerical trouble.
  bool insert_all(std::size_t real_count);
  // Makes (a, b) an edge and marks it constrained (and a wall of the domain
  // when `wall`). False when a vertex lies on the segment or recovery
  // doesn't converge.
  bool recover(std::uint32_t a, std::uint32_t b, bool wall);
  // Restores the Delaunay property away from constrained edges.
  bool legalise_all();
  // Triangles inside the constraint loops (even-odd from the outside).
  std::vector<Triangle> inside(std::size_t real_count) const;

 private:
  bool constrained(std::uint32_t a, std::uint32_t b) const {
    return constrained_.contains(edge_key(a, b));
  }
  std::uint32_t corner(std::uint32_t t, std::uint32_t v) const {
    const Tri& tr = t_[t];
    return tr.v[0] == v ? 0 : (tr.v[1] == v ? 1 : (tr.v[2] == v ? 2 : kNone));
  }
  void replace_neighbor(std::uint32_t t, std::uint32_t old_n, std::uint32_t new_n) {
    if (t == kNone) return;
    for (auto& n : t_[t].n) {
      if (n == old_n) {
        n = new_n;
        return;
      }
    }
  }
  void touch(std::uint32_t t) {
    for (const std::uint32_t v : t_[t].v) vt_[v] = t;
  }
  std::uint32_t add(const Tri& tr) {
    t_.push_back(tr);
    return static_cast<std::uint32_t>(t_.size() - 1);
  }
  // Locates q: the triangle containing it, and the edge it lies on (kNone
  // when strictly inside). False when the walk fails.
  bool locate(const dvec2& q, std::uint32_t& tri, std::uint32_t& edge) const;
  bool insert(std::uint32_t q);
  bool convex_flip_ok(std::uint32_t t, std::uint32_t k) const;
  void flip(std::uint32_t t, std::uint32_t k);
  // False when it takes more flips than any Delaunay insertion can need
  // (rounding cycling flips): the triangulation then fails, never hangs.
  bool legalise(std::vector<std::pair<std::uint32_t, std::uint32_t>>& stack);
  // The triangle holding directed or undirected edge (x, y), and the edge's
  // index in it; kNone when the edge doesn't exist.
  std::pair<std::uint32_t, std::uint32_t> find_edge(std::uint32_t x, std::uint32_t y) const;

  std::vector<dvec2> p_;
  std::vector<Tri> t_;
  std::vector<std::uint32_t> vt_;  // a triangle at each vertex
  std::unordered_set<std::uint64_t> constrained_;
  std::unordered_set<std::uint64_t> walls_;  // loop segments: the domain's boundary
  std::uint32_t last_ = 0;
};

bool Cdt::locate(const dvec2& q, std::uint32_t& tri, std::uint32_t& edge) const {
  std::uint32_t cur = last_ < t_.size() ? last_ : 0;
  const std::size_t limit = 4 * t_.size() + 16;
  for (std::size_t step = 0; step < limit; ++step) {
    const Tri& tr = t_[cur];
    bool moved = false;
    // Start from a different edge each step, so the walk can't cycle.
    for (std::uint32_t i = 0; i < 3 && !moved; ++i) {
      const std::uint32_t k = static_cast<std::uint32_t>((i + step) % 3);
      const dvec2& a = p_[tr.v[inc(k)]];
      const dvec2& b = p_[tr.v[dec(k)]];
      // Across any edge q is beyond, however slightly: orient() is exactly
      // antisymmetric, so the two triangles of an edge never both send q
      // back, and the walk ends in the triangle that holds q (H4: with a
      // tolerance here it could stop in a flat neighbour q only lines up
      // with).
      if (orient(a, b, q) < 0 && tr.n[k] != kNone) {
        cur = tr.n[k];
        moved = true;
      }
    }
    if (moved) continue;
    tri = cur;
    edge = kNone;
    // The edges q lies on, within rounding. In a sliver (three nearly
    // collinear corners) q can be within rounding of two edges without
    // being near their shared corner: it lies on the one it projects
    // inside of, the nearer if both (H4: this failed as a "duplicate").
    std::uint32_t on = 0;
    double best = std::numeric_limits<double>::infinity();
    for (std::uint32_t k = 0; k < 3; ++k) {
      const dvec2& a = p_[tr.v[inc(k)]];
      const dvec2& b = p_[tr.v[dec(k)]];
      const double o = orient(a, b, q);
      if (std::abs(o) > orient_eps(a, b, q)) {
        if (o < 0) return false;  // outside the super triangle
        continue;
      }
      ++on;
      const dvec2 ab = b - a;
      const double len2 = core::dot(ab, ab);
      const double t = core::dot(q - a, ab) / len2;
      if (!(t > 0 && t < 1)) continue;
      const double off = std::abs(o) / std::sqrt(len2);
      if (off < best) {
        best = off;
        edge = k;
      }
    }
    if (on > 0 && edge == kNone) return false;  // at a corner: a duplicate point
    return true;
  }
  return false;
}

bool Cdt::insert(std::uint32_t q) {
  std::uint32_t cur = 0, k = kNone;
  if (!locate(p_[q], cur, k)) return false;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> stack;
  if (k == kNone) {
    const Tri old = t_[cur];
    const std::uint32_t a = old.v[0], b = old.v[1], c = old.v[2];
    const std::uint32_t na = old.n[0], nb = old.n[1], nc = old.n[2];
    const std::uint32_t t0 = cur;
    const auto t1 = static_cast<std::uint32_t>(t_.size());
    const std::uint32_t t2 = t1 + 1;
    t_[t0] = Tri{{a, b, q}, {t1, t2, nc}};
    add(Tri{{b, c, q}, {t2, t0, na}});
    add(Tri{{c, a, q}, {t0, t1, nb}});
    replace_neighbor(na, cur, t1);
    replace_neighbor(nb, cur, t2);
    for (const std::uint32_t t : {t0, t1, t2}) {
      touch(t);
      stack.emplace_back(t, q);
    }
    last_ = t0;
  } else {
    const Tri old = t_[cur];
    const std::uint32_t c = old.v[k], a = old.v[inc(k)], b = old.v[dec(k)];
    const std::uint32_t u = old.n[k];
    if (u == kNone) return false;
    const Tri uo = t_[u];
    const std::uint32_t ua = corner(u, a), ub = corner(u, b);
    if (ua == kNone || ub == kNone) return false;
    const std::uint32_t j = 3 - ua - ub;
    const std::uint32_t d = uo.v[j];
    const std::uint32_t n_ca = old.n[dec(k)];  // opposite b
    const std::uint32_t n_bc = old.n[inc(k)];  // opposite a
    const std::uint32_t n_db = uo.n[dec(j)];   // opposite a in u
    const std::uint32_t n_ad = uo.n[inc(j)];   // opposite b in u
    const std::uint32_t T0 = cur, U0 = u;
    const auto T1 = static_cast<std::uint32_t>(t_.size());
    const std::uint32_t U1 = T1 + 1;
    t_[T0] = Tri{{c, a, q}, {U1, T1, n_ca}};
    add(Tri{{c, q, b}, {U0, n_bc, T0}});
    t_[U0] = Tri{{d, b, q}, {T1, U1, n_db}};
    add(Tri{{d, q, a}, {T0, n_ad, U0}});
    replace_neighbor(n_bc, cur, T1);
    replace_neighbor(n_ad, u, U1);
    for (const std::uint32_t t : {T0, T1, U0, U1}) {
      touch(t);
      stack.emplace_back(t, q);
    }
    last_ = T0;
  }
  return legalise(stack);
}

bool Cdt::convex_flip_ok(std::uint32_t t, std::uint32_t k) const {
  const Tri& tr = t_[t];
  const std::uint32_t u = tr.n[k];
  if (u == kNone) return false;
  const std::uint32_t p = tr.v[k], a = tr.v[inc(k)], b = tr.v[dec(k)];
  const std::uint32_t ca = corner(u, a), cb = corner(u, b);
  if (ca == kNone || cb == kNone) return false;
  const std::uint32_t q = t_[u].v[3 - ca - cb];
  const double o1 = orient(p_[p], p_[a], p_[q]);
  const double o2 = orient(p_[q], p_[b], p_[p]);
  return o1 > orient_eps(p_[p], p_[a], p_[q]) && o2 > orient_eps(p_[q], p_[b], p_[p]);
}

void Cdt::flip(std::uint32_t t, std::uint32_t k) {
  const Tri to = t_[t];
  const std::uint32_t u = to.n[k];
  const Tri uo = t_[u];
  const std::uint32_t p = to.v[k], a = to.v[inc(k)], b = to.v[dec(k)];
  const std::uint32_t j = 3 - corner(u, a) - corner(u, b);
  const std::uint32_t q = uo.v[j];
  const std::uint32_t n_pa = to.n[dec(k)];  // opposite b
  const std::uint32_t n_bp = to.n[inc(k)];  // opposite a
  const std::uint32_t n_qb = uo.n[dec(j)];  // opposite a in u
  const std::uint32_t n_aq = uo.n[inc(j)];  // opposite b in u
  t_[t] = Tri{{p, a, q}, {n_aq, u, n_pa}};
  t_[u] = Tri{{q, b, p}, {n_bp, t, n_qb}};
  replace_neighbor(n_aq, u, t);
  replace_neighbor(n_bp, t, u);
  touch(t);
  touch(u);
}

bool Cdt::legalise(std::vector<std::pair<std::uint32_t, std::uint32_t>>& stack) {
  // An insertion's flips each add a triangle to the new point's star, which
  // has fewer triangles than the triangulation.
  const std::size_t limit = 2 * t_.size() + 64;
  std::size_t flips = 0;
  while (!stack.empty()) {
    const auto [t, q] = stack.back();
    stack.pop_back();
    const std::uint32_t k = corner(t, q);
    if (k == kNone) continue;
    const Tri& tr = t_[t];
    const std::uint32_t u = tr.n[k];
    if (u == kNone || constrained(tr.v[inc(k)], tr.v[dec(k)])) continue;
    const std::uint32_t ca = corner(u, tr.v[inc(k)]), cb = corner(u, tr.v[dec(k)]);
    if (ca == kNone || cb == kNone) continue;
    const std::uint32_t d = t_[u].v[3 - ca - cb];
    if (incircle(p_[tr.v[0]], p_[tr.v[1]], p_[tr.v[2]], p_[d]) <= 0) continue;
    if (!convex_flip_ok(t, k)) continue;
    if (++flips > limit) return false;
    flip(t, k);
    stack.emplace_back(t, q);
    stack.emplace_back(u, q);
  }
  return true;
}

bool Cdt::insert_all(std::size_t real_count) {
  // Super triangle round every point.
  dvec2 lo = p_[0], hi = p_[0];
  for (std::size_t i = 0; i < real_count; ++i) {
    lo = core::min(lo, p_[i]);
    hi = core::max(hi, p_[i]);
  }
  const dvec2 c = (lo + hi) * 0.5;
  // Scaled to the points, whatever their size (a fixed floor made points
  // under a nanometre apart all lie "on" the super triangle's edges).
  double d = std::max(hi.x - lo.x, hi.y - lo.y);
  if (!(d > 0)) d = std::max({std::abs(c.x), std::abs(c.y), 1.0});
  p_.resize(real_count);
  p_.push_back({c.x - 20 * d, c.y - 10 * d});
  p_.push_back({c.x + 20 * d, c.y - 10 * d});
  p_.push_back({c.x, c.y + 20 * d});
  const auto s = static_cast<std::uint32_t>(real_count);
  vt_.assign(p_.size(), kNone);
  t_.clear();
  add(Tri{{s, s + 1, s + 2}, {kNone, kNone, kNone}});
  touch(0);
  for (std::uint32_t i = 0; i < real_count; ++i) {
    if (!insert(i)) return false;
  }
  return true;
}

std::pair<std::uint32_t, std::uint32_t> Cdt::find_edge(std::uint32_t x, std::uint32_t y) const {
  // Rotate round x; fall back to a scan if the fan is open.
  std::uint32_t t = vt_[x];
  for (std::size_t i = 0; i < t_.size() && t != kNone; ++i) {
    const std::uint32_t kx = corner(t, x);
    if (kx == kNone) break;
    const std::uint32_t ky = corner(t, y);
    if (ky != kNone) return {t, 3 - kx - ky};
    const std::uint32_t next = t_[t].n[inc(kx)];
    if (next == vt_[x]) return {kNone, kNone};
    t = next;
  }
  for (std::uint32_t s = 0; s < t_.size(); ++s) {
    const std::uint32_t kx = corner(s, x), ky = corner(s, y);
    if (kx != kNone && ky != kNone) return {s, 3 - kx - ky};
  }
  return {kNone, kNone};
}

bool Cdt::recover(std::uint32_t a, std::uint32_t b, bool wall) {
  if (wall) walls_.insert(edge_key(a, b));
  if (find_edge(a, b).first != kNone) {
    constrained_.insert(edge_key(a, b));
    return true;
  }
  const dvec2& pa = p_[a];
  const dvec2& pb = p_[b];
  // The triangle at a that the segment leaves through.
  std::uint32_t t = vt_[a];
  std::uint32_t c1 = kNone, c2 = kNone;
  for (std::size_t i = 0; i <= t_.size(); ++i) {
    const std::uint32_t k = corner(t, a);
    if (k == kNone) return false;
    const std::uint32_t v1 = t_[t].v[inc(k)], v2 = t_[t].v[dec(k)];
    const double o1 = orient(pa, p_[v1], pb);
    const double o2 = orient(pa, pb, p_[v2]);
    if (std::abs(o1) <= orient_eps(pa, p_[v1], pb) &&
        core::dot(p_[v1] - pa, pb - pa) > 0) {
      return false;  // a vertex on the segment
    }
    if (o1 > 0 && o2 > 0) {
      c1 = v1;
      c2 = v2;
      break;
    }
    t = t_[t].n[inc(k)];
    if (t == kNone) return false;
  }
  if (c1 == kNone) return false;

  std::deque<std::pair<std::uint32_t, std::uint32_t>> crossing;
  crossing.emplace_back(c1, c2);
  // Walk the triangles the segment passes through; (c1, c2) is the edge of
  // `cur` opposite `opp` that the segment leaves through, c1 to its right.
  std::uint32_t cur = t, opp = a;
  for (std::size_t guard = 0;; ++guard) {
    if (guard > t_.size()) return false;
    const std::uint32_t ko = corner(cur, opp);
    if (ko == kNone) return false;
    const std::uint32_t u = t_[cur].n[ko];
    if (u == kNone) return false;
    const std::uint32_t k1 = corner(u, c1), k2 = corner(u, c2);
    if (k1 == kNone || k2 == kNone) return false;
    const std::uint32_t w = t_[u].v[3 - k1 - k2];
    if (w == b) break;
    const double ow = orient(pa, pb, p_[w]);
    if (std::abs(ow) <= orient_eps(pa, pb, p_[w])) return false;  // vertex on segment
    if (ow > 0) {
      opp = c2;
      c2 = w;
    } else {
      opp = c1;
      c1 = w;
    }
    cur = u;
    crossing.emplace_back(c1, c2);
  }

  const auto crosses = [&](std::uint32_t x, std::uint32_t y) {
    if (x == a || x == b || y == a || y == b) return false;
    // Signs, never products: a product of two tiny areas can round to zero.
    const auto opposite = [](double p, double q) { return (p < 0 && q > 0) || (p > 0 && q < 0); };
    return opposite(orient(pa, pb, p_[x]), orient(pa, pb, p_[y])) &&
           opposite(orient(p_[x], p_[y], pa), orient(p_[x], p_[y], pb));
  };
  const std::size_t limit = 64 * crossing.size() * crossing.size() + 1024;
  for (std::size_t guard = 0; !crossing.empty(); ++guard) {
    if (guard > limit) return false;
    const auto [x, y] = crossing.front();
    crossing.pop_front();
    const auto [et, ek] = find_edge(x, y);
    if (et == kNone) return false;
    // A constrained edge in the way means the segments cross after all
    // (within rounding): fail rather than flip an earlier segment away.
    if (constrained(x, y)) return false;
    if (!convex_flip_ok(et, ek)) {
      crossing.emplace_back(x, y);
      continue;
    }
    const std::uint32_t p = t_[et].v[ek];
    const std::uint32_t u = t_[et].n[ek];
    const std::uint32_t k1 = corner(u, x), k2 = corner(u, y);
    const std::uint32_t q = t_[u].v[3 - k1 - k2];
    flip(et, ek);
    if (crosses(p, q)) crossing.emplace_back(p, q);
  }
  if (find_edge(a, b).first == kNone) return false;
  constrained_.insert(edge_key(a, b));
  return true;
}

bool Cdt::legalise_all() {
  for (int pass = 0; pass < 10000; ++pass) {
    bool flipped = false;
    for (std::uint32_t t = 0; t < t_.size(); ++t) {
      for (std::uint32_t k = 0; k < 3; ++k) {
        const Tri& tr = t_[t];
        const std::uint32_t u = tr.n[k];
        if (u == kNone || u < t) continue;
        const std::uint32_t a = tr.v[inc(k)], b = tr.v[dec(k)];
        if (constrained(a, b)) continue;
        const std::uint32_t ca = corner(u, a), cb = corner(u, b);
        const std::uint32_t d = t_[u].v[3 - ca - cb];
        if (incircle(p_[tr.v[0]], p_[tr.v[1]], p_[tr.v[2]], p_[d]) <= 0) continue;
        if (!convex_flip_ok(t, k)) continue;
        flip(t, k);
        flipped = true;
      }
    }
    if (!flipped) return true;
  }
  return false;
}

std::vector<Triangle> Cdt::inside(std::size_t real_count) const {
  std::vector<int> depth(t_.size(), -1);
  std::deque<std::uint32_t> queue;
  for (std::uint32_t t = 0; t < t_.size(); ++t) {
    for (const std::uint32_t v : t_[t].v) {
      if (v >= real_count && depth[t] < 0) {
        depth[t] = 0;
        queue.push_back(t);
      }
    }
  }
  // 0-1 BFS: crossing a constrained edge adds one.
  while (!queue.empty()) {
    const std::uint32_t t = queue.front();
    queue.pop_front();
    for (std::uint32_t k = 0; k < 3; ++k) {
      const std::uint32_t u = t_[t].n[k];
      if (u == kNone) continue;
      const bool wall = walls_.contains(edge_key(t_[t].v[inc(k)], t_[t].v[dec(k)]));
      const int d = depth[t] + (wall ? 1 : 0);
      if (depth[u] < 0 || d < depth[u]) {
        depth[u] = d;
        if (wall) {
          queue.push_back(u);
        } else {
          queue.push_front(u);
        }
      }
    }
  }
  std::vector<Triangle> out;
  for (std::uint32_t t = 0; t < t_.size(); ++t) {
    if (depth[t] % 2 == 1) out.push_back(t_[t].v);
  }
  return out;
}

bool segments_touch(const dvec2& p0, const dvec2& p1, const dvec2& q0, const dvec2& q1) {
  const double d1 = orient(p0, p1, q0), d2 = orient(p0, p1, q1);
  const double d3 = orient(q0, q1, p0), d4 = orient(q0, q1, p1);
  if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
    return true;
  }
  const auto on = [](const dvec2& a, const dvec2& b, const dvec2& c, double o) {
    return o == 0 && std::min(a.x, b.x) <= c.x && c.x <= std::max(a.x, b.x) &&
           std::min(a.y, b.y) <= c.y && c.y <= std::max(a.y, b.y);
  };
  return on(p0, p1, q0, d1) || on(p0, p1, q1, d2) || on(q0, q1, p0, d3) || on(q0, q1, p1, d4);
}

// True when two segments (of loops, or extra ones) meet anywhere they
// shouldn't. Candidate pairs come from a bounding-volume tree over the
// segments' boxes, so outlines mixing long and short segments cost
// O(M log M + pairs whose boxes touch), whatever their scales (H4; a hash
// grid sized by the mean segment length ran out of memory on them).
bool check_segments(std::span<const dvec2> pts, const std::vector<Loop>& loops,
                    std::span<const Segment> extra) {
  struct Seg {
    std::uint32_t a, b;
    dvec2 lo, hi;
  };
  std::vector<Seg> segs;
  const auto add = [&](std::uint32_t a, std::uint32_t b) {
    segs.push_back({a, b, core::min(pts[a], pts[b]), core::max(pts[a], pts[b])});
  };
  for (const Loop& loop : loops) {
    for (std::size_t i = 0; i < loop.size(); ++i) add(loop[i], loop[(i + 1) % loop.size()]);
  }
  for (const Segment& e : extra) add(e[0], e[1]);
  if (segs.size() < 2) return false;

  // Two segments meeting where they shouldn't.
  const auto bad_pair = [&](std::uint32_t s, std::uint32_t t) {
    const Seg& p = segs[s];
    const Seg& q = segs[t];
    std::uint32_t shared = kNone;
    if (p.a == q.a || p.a == q.b) shared = p.a;
    if (p.b == q.a || p.b == q.b) shared = shared == kNone ? p.b : kNone - 1;
    if (shared == kNone - 1) return true;  // the same segment twice
    if (shared != kNone) {
      const std::uint32_t pe = p.a == shared ? p.b : p.a;
      const std::uint32_t qe = q.a == shared ? q.b : q.a;
      // Adjacent segments may only meet at their shared end: not fold back.
      const dvec2& v = pts[shared];
      return orient(v, pts[pe], pts[qe]) == 0 && core::dot(pts[pe] - v, pts[qe] - v) > 0;
    }
    return segments_touch(pts[p.a], pts[p.b], pts[q.a], pts[q.b]);
  };

  // The tree: nodes over ranges of `order`, split at the median centre
  // along the wider axis; leaves of up to four segments.
  struct Node {
    dvec2 lo, hi;
    std::uint32_t begin = 0, end = 0;
    std::uint32_t left = kNone, right = kNone;
  };
  std::vector<std::uint32_t> order(segs.size());
  for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::vector<Node> nodes;
  nodes.reserve(segs.size());
  std::vector<std::uint32_t> pending{0};
  nodes.push_back({{}, {}, 0, static_cast<std::uint32_t>(segs.size())});
  while (!pending.empty()) {
    const std::uint32_t n = pending.back();
    pending.pop_back();
    const std::uint32_t begin = nodes[n].begin, end = nodes[n].end;
    dvec2 lo = segs[order[begin]].lo, hi = segs[order[begin]].hi;
    dvec2 clo = (segs[order[begin]].lo + segs[order[begin]].hi) * 0.5, chi = clo;
    for (std::uint32_t i = begin; i < end; ++i) {
      const Seg& g = segs[order[i]];
      lo = core::min(lo, g.lo);
      hi = core::max(hi, g.hi);
      const dvec2 c = (g.lo + g.hi) * 0.5;
      clo = core::min(clo, c);
      chi = core::max(chi, c);
    }
    nodes[n].lo = lo;
    nodes[n].hi = hi;
    if (end - begin <= 4) continue;
    const bool by_x = chi.x - clo.x >= chi.y - clo.y;
    const std::uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end,
                     [&](std::uint32_t a, std::uint32_t b) {
                       const double ca = by_x ? segs[a].lo.x + segs[a].hi.x : segs[a].lo.y + segs[a].hi.y;
                       const double cb = by_x ? segs[b].lo.x + segs[b].hi.x : segs[b].lo.y + segs[b].hi.y;
                       return ca != cb ? ca < cb : a < b;
                     });
    const auto left = static_cast<std::uint32_t>(nodes.size());
    nodes.push_back({{}, {}, begin, mid});
    nodes.push_back({{}, {}, mid, end});
    nodes[n].left = left;
    nodes[n].right = left + 1;
    pending.push_back(left);
    pending.push_back(left + 1);
  }

  const auto overlaps = [](const dvec2& alo, const dvec2& ahi, const dvec2& blo, const dvec2& bhi) {
    return alo.x <= bhi.x && blo.x <= ahi.x && alo.y <= bhi.y && blo.y <= ahi.y;
  };
  std::vector<std::uint32_t> stack;
  for (std::uint32_t s = 0; s < segs.size(); ++s) {
    stack.assign(1, 0);
    while (!stack.empty()) {
      const Node& node = nodes[stack.back()];
      stack.pop_back();
      if (!overlaps(node.lo, node.hi, segs[s].lo, segs[s].hi)) continue;
      if (node.left == kNone) {
        for (std::uint32_t i = node.begin; i < node.end; ++i) {
          const std::uint32_t t = order[i];
          if (t <= s || !overlaps(segs[t].lo, segs[t].hi, segs[s].lo, segs[s].hi)) continue;
          if (bad_pair(s, t)) return true;
        }
        continue;
      }
      stack.push_back(node.left);
      stack.push_back(node.right);
    }
  }
  return false;
}

// Even-odd test against every loop.
bool inside_loops(std::span<const dvec2> pts, const std::vector<Loop>& loops, const dvec2& q) {
  bool in = false;
  for (const Loop& loop : loops) {
    for (std::size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
      const dvec2& a = pts[loop[i]];
      const dvec2& b = pts[loop[j]];
      if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x) {
        in = !in;
      }
    }
  }
  return in;
}

}  // namespace

double signed_double_area(std::span<const core::dvec2> points, const Loop& loop) {
  double a = 0;
  for (std::size_t i = 0; i < loop.size(); ++i) {
    const dvec2& p = points[loop[i]];
    const dvec2& q = points[loop[(i + 1) % loop.size()]];
    a += p.x * q.y - q.x * p.y;
  }
  return a;
}

core::Result<std::vector<Triangle>> triangulate(std::span<const core::dvec2> points,
                                                const std::vector<Loop>& loops) {
  return triangulate(points, loops, {});
}

core::Result<std::vector<Triangle>> triangulate(std::span<const core::dvec2> points,
                                                const std::vector<Loop>& loops,
                                                std::span<const Segment> segments) {
  if (loops.empty()) return error("mesh.triangulate.input", "No outer loop");
  for (const Segment& e : segments) {
    if (e[0] >= points.size() || e[1] >= points.size() || e[0] == e[1]) {
      return error("mesh.triangulate.input", "A segment's index is out of range or repeated");
    }
  }
  std::vector<int> on_loop(points.size(), 0);
  for (const Loop& loop : loops) {
    if (loop.size() < 3) return error("mesh.triangulate.input", "A loop has fewer than three points");
    for (const std::uint32_t i : loop) {
      if (i >= points.size()) return error("mesh.triangulate.input", "A loop index is out of range");
      if (on_loop[i]++ > 0) {
        return error("mesh.triangulate.duplicate_point",
                     "Point " + std::to_string(i) + " is on a loop twice");
      }
    }
  }
  for (const dvec2& p : points) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
      return error("mesh.triangulate.input", "A point is not finite");
    }
  }
  {
    std::vector<std::uint32_t> order(points.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
      return points[a].x != points[b].x ? points[a].x < points[b].x
                                        : (points[a].y != points[b].y ? points[a].y < points[b].y
                                                                      : a < b);
    });
    for (std::size_t i = 1; i < order.size(); ++i) {
      if (points[order[i]] == points[order[i - 1]]) {
        return error("mesh.triangulate.duplicate_point",
                     "Points " + std::to_string(order[i - 1]) + " and " +
                         std::to_string(order[i]) + " coincide");
      }
    }
  }
  if (check_segments(points, loops, segments)) {
    return error("mesh.triangulate.self_intersecting", "Loop segments cross or touch");
  }
  for (std::size_t h = 1; h < loops.size(); ++h) {
    const dvec2& q = points[loops[h][0]];
    bool ok = inside_loops(points, {loops[0]}, q);
    for (std::size_t o = 1; o < loops.size() && ok; ++o) {
      if (o != h && inside_loops(points, {loops[o]}, q)) ok = false;
    }
    if (!ok) {
      return error("mesh.triangulate.input",
                   "Hole " + std::to_string(h) + " is not inside the outer loop, or is inside another hole");
    }
  }

  // Insertion order: biased randomized insertion (rounds of doubling size,
  // each in Z-order), which keeps flips and point-location walks short even
  // for long runs of collinear points. The shuffle is our own (splitmix64),
  // so the order, and the result, is the same on every platform.
  std::vector<std::uint32_t> order(points.size());
  for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  {
    std::uint64_t state = 0x9e3779b97f4a7c15ull ^ points.size();
    const auto next = [&state] {
      std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
      z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
      return z ^ (z >> 31);
    };
    for (std::size_t i = order.size(); i > 1; --i) {
      std::swap(order[i - 1], order[static_cast<std::size_t>(next() % i)]);
    }
    dvec2 lo = points[0], hi = points[0];
    for (const dvec2& p : points) {
      lo = core::min(lo, p);
      hi = core::max(hi, p);
    }
    const double w = std::max({hi.x - lo.x, hi.y - lo.y, 1e-300});
    const auto morton = [&](std::uint32_t i) {
      const auto q = [&](double v, double v0) {
        return static_cast<std::uint64_t>(std::clamp((v - v0) / w, 0.0, 1.0) * 65535.0);
      };
      std::uint64_t x = q(points[i].x, lo.x), y = q(points[i].y, lo.y), code = 0;
      for (int b = 0; b < 16; ++b) {
        code |= ((x >> b) & 1u) << (2 * b);
        code |= ((y >> b) & 1u) << (2 * b + 1);
      }
      return code;
    };
    std::vector<std::uint64_t> codes(points.size());
    for (std::uint32_t i = 0; i < points.size(); ++i) codes[i] = morton(i);
    const auto by_code = [&](std::uint32_t a, std::uint32_t b) {
      return codes[a] != codes[b] ? codes[a] < codes[b] : a < b;
    };
    // Rounds: [0, n/2^k), ..., [n/4, n/2), [n/2, n).
    std::size_t end = order.size();
    while (end > 0) {
      const std::size_t begin = end > 64 ? end / 2 : 0;
      std::sort(order.begin() + static_cast<std::ptrdiff_t>(begin),
                order.begin() + static_cast<std::ptrdiff_t>(end), by_code);
      end = begin;
    }
  }
  std::vector<dvec2> ordered(points.size());
  for (std::size_t i = 0; i < order.size(); ++i) ordered[i] = points[order[i]];
  std::vector<std::uint32_t> rank(points.size());
  for (std::uint32_t i = 0; i < order.size(); ++i) rank[order[i]] = i;

  Cdt cdt(ordered);
  const auto failed = [](std::string what) {
    return error("mesh.triangulate.failed", "Triangulation failed: " + what);
  };
  if (!cdt.insert_all(points.size())) return failed("point insertion");
  for (const Loop& loop : loops) {
    for (std::size_t i = 0; i < loop.size(); ++i) {
      if (!cdt.recover(rank[loop[i]], rank[loop[(i + 1) % loop.size()]], true)) {
        return failed("edge recovery");
      }
    }
  }
  for (const Segment& e : segments) {
    if (!cdt.recover(rank[e[0]], rank[e[1]], false)) return failed("edge recovery");
  }
  if (!cdt.legalise_all()) return failed("Delaunay flips did not converge");
  std::vector<Triangle> tris = cdt.inside(points.size());
  std::vector<bool> used(points.size(), false);
  for (Triangle& t : tris) {
    for (std::uint32_t& v : t) {
      v = order[v];
      used[v] = true;
    }
    if (orient(points[t[0]], points[t[1]], points[t[2]]) <= 0) {
      return failed("a triangle is inverted or flat");
    }
  }
  // A Steiner point outside the domain (or in a hole) lands in a triangle
  // that was removed; checking here costs nothing, unlike a point-in-polygon
  // test per point beforehand.
  for (std::uint32_t i = 0; i < points.size(); ++i) {
    if (used[i]) continue;
    if (on_loop[i] == 0) {
      return error("mesh.triangulate.steiner_outside",
                   "Point " + std::to_string(i) + " is not inside the polygon");
    }
    return failed("point " + std::to_string(i) + " was left out");
  }
  // A canonical order: by smallest corner, rotated to start there.
  for (Triangle& t : tris) {
    while (t[0] > t[1] || t[0] > t[2]) t = {t[1], t[2], t[0]};
  }
  std::sort(tris.begin(), tris.end());
  return tris;
}

}  // namespace vs::mesh

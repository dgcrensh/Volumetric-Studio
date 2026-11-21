#include "vs/sketch/profile.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>
#include <numeric>
#include <optional>

#include <fmt/format.h>

#include "vs/kernel/primitives.hpp"

namespace vs::sketch {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
// Chords per full turn when an arc is turned into a polygon for containment
// tests (not for faces, which keep exact arcs).
constexpr int kSamplesPerTurn = 256;
// The most chords per turn deflection asks for (an arc of 1,000 mm radius
// within 1e-3 mm needs about 2,200).
constexpr double kMaxSamplesPerTurn = 65536;

// A curve that can be part of a loop, resolved to sketch coordinates.
struct Curve {
  Id id;
  EntityKind kind = EntityKind::Line;
  LineGeometry line{};
  ArcGeometry arc{};
  CircleGeometry circle{};
  // End points (lines and arcs): point ids and positions.
  Id end_ids[2];
  dvec2 ends[2];
};

std::vector<Curve> profile_curves(const Sketch& sketch) {
  std::vector<Curve> out;
  for (const auto& e : sketch.entities()) {
    if (e.construction) continue;
    Curve c;
    c.id = e.id;
    c.kind = e.kind();
    if (const auto* l = e.as<Line>()) {
      c.line = *line_geometry(sketch, e.id);
      c.end_ids[0] = l->start;
      c.end_ids[1] = l->end;
      c.ends[0] = c.line.start;
      c.ends[1] = c.line.end;
    } else if (const auto* a = e.as<Arc>()) {
      c.arc = *arc_geometry(sketch, e.id);
      c.end_ids[0] = a->start;
      c.end_ids[1] = a->end;
      c.ends[0] = c.arc.start();
      c.ends[1] = c.arc.end();
    } else if (e.as<Circle>() != nullptr) {
      c.circle = *circle_geometry(sketch, e.id);
    } else {
      continue;
    }
    out.push_back(c);
  }
  return out;
}

// Signed area contribution of one edge, ∮ (x dy − y dx) / 2.
double edge_area(const Curve& c, bool reversed) {
  double a = 0;
  if (c.kind == EntityKind::Line) {
    a = 0.5 * core::cross(c.line.start, c.line.end);
  } else if (c.kind == EntityKind::Arc) {
    const auto& g = c.arc;
    auto f = [&](double t) {
      return 0.5 * (g.radius * (g.center.x * std::sin(t) - g.center.y * std::cos(t)) +
                    g.radius * g.radius * t);
    };
    a = f(g.end_angle) - f(g.start_angle);
  } else {
    a = std::numbers::pi * c.circle.radius * c.circle.radius;
  }
  return reversed ? -a : a;
}

// Points along an edge in traversal order, excluding its last point. Arcs
// get at least kSamplesPerTurn chords a turn, and more on large radii, so
// that no chord strays more than `deflection` from the arc.
void sample_edge(const Curve& c, bool reversed, double deflection, std::vector<dvec2>& out) {
  if (c.kind == EntityKind::Line) {
    out.push_back(reversed ? c.line.end : c.line.start);
    return;
  }
  ArcGeometry g;
  if (c.kind == EntityKind::Arc) {
    g = c.arc;
  } else {
    g = {c.circle.center, c.circle.radius, 0.0, kTwoPi};
  }
  const double sweep = g.end_angle - g.start_angle;
  double per_turn = kSamplesPerTurn;
  if (g.radius > deflection) {
    // A chord spanning angle a strays r (1 - cos(a / 2)) from its arc.
    const double a = 2.0 * std::acos(1.0 - deflection / g.radius);
    if (a > 0.0) per_turn = std::max(per_turn, std::ceil(kTwoPi / a));
  }
  per_turn = std::min(per_turn, kMaxSamplesPerTurn);
  const int n = std::max(4, static_cast<int>(std::ceil(sweep / kTwoPi * per_turn)));
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(reversed ? n - i : i) / n;
    out.push_back(g.at(g.start_angle + t * sweep));
  }
}

struct Box2 {
  dvec2 min{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
  dvec2 max{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
  void expand(const dvec2& p) {
    min = {std::min(min.x, p.x), std::min(min.y, p.y)};
    max = {std::max(max.x, p.x), std::max(max.y, p.y)};
  }
  [[nodiscard]] bool intersects(const Box2& o, double slack) const {
    return min.x <= o.max.x + slack && o.min.x <= max.x + slack && min.y <= o.max.y + slack &&
           o.min.y <= max.y + slack;
  }
  [[nodiscard]] bool contains(const dvec2& p, double slack) const {
    return p.x >= min.x - slack && p.x <= max.x + slack && p.y >= min.y - slack &&
           p.y <= max.y + slack;
  }
};

struct Loop {
  std::vector<LoopEdge> edges;
  double signed_area = 0;
  std::vector<dvec2> polygon;
  Box2 box;  // the polygon's
};

// Even-odd point-in-polygon.
bool inside(const std::vector<dvec2>& poly, const dvec2& p) {
  bool in = false;
  for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const dvec2& a = poly[i];
    const dvec2& b = poly[j];
    if ((a.y > p.y) != (b.y > p.y)) {
      const double x = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
      if (p.x < x) in = !in;
    }
  }
  return in;
}

double distance_to_polygon(const std::vector<dvec2>& poly, const dvec2& p) {
  double best = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const dvec2 ab = poly[i] - poly[j];
    const double len2 = core::dot(ab, ab);
    double t = len2 > 0 ? core::dot(p - poly[j], ab) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    best = std::min(best, core::distance(p, poly[j] + ab * t));
  }
  return best;
}

// One side of a polygon: its two points and box.
struct Segment {
  dvec2 a;
  dvec2 b;
  std::size_t index;  // within its polygon
  double min_x;
  double max_x;
};

std::vector<Segment> segments_by_x(const std::vector<dvec2>& poly) {
  std::vector<Segment> out;
  out.reserve(poly.size());
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const dvec2& a = poly[i];
    const dvec2& b = poly[(i + 1) % poly.size()];
    out.push_back({a, b, i, std::min(a.x, b.x), std::max(a.x, b.x)});
  }
  std::ranges::sort(out, [](const Segment& s, const Segment& t) {
    return s.min_x != t.min_x ? s.min_x < t.min_x : s.index < t.index;
  });
  return out;
}

// Signed distance of `p` from the line through `a` and `b` (left positive).
double side(const dvec2& a, const dvec2& b, const dvec2& p) {
  const double len = core::length(b - a);
  if (len <= 0.0) return 0.0;
  return core::cross(b - a, p - a) / len;
}

// True when the two segments cross each other transversally, each end lying
// more than `slack` off the other's line: touching, overlapping and meeting
// at an end are not crossings.
bool crosses(const Segment& s, const Segment& t, double slack) {
  const double d1 = side(s.a, s.b, t.a);
  const double d2 = side(s.a, s.b, t.b);
  if (!((d1 > slack && d2 < -slack) || (d1 < -slack && d2 > slack))) return false;
  const double d3 = side(t.a, t.b, s.a);
  const double d4 = side(t.a, t.b, s.b);
  return (d3 > slack && d4 < -slack) || (d3 < -slack && d4 > slack);
}

// The first crossing between two polygons' sides (`same`: one polygon with
// itself, neighbouring sides skipped). A sweep in x: sides enter in order of
// their smallest x and meet only the active sides whose x range reaches
// theirs, so it costs about the number of sides times the few a vertical
// line meets.
bool any_crossing(const std::vector<Segment>& p, const std::vector<Segment>* q, double slack,
                  std::size_t sides) {
  const bool same = q == nullptr;
  // Both lists merged by smallest x: (from q, index in its list).
  std::vector<std::pair<bool, std::size_t>> order;
  order.reserve(p.size() + (same ? 0 : q->size()));
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < p.size() || (!same && j < q->size())) {
    if (same || j >= q->size() || (i < p.size() && p[i].min_x <= (*q)[j].min_x)) {
      order.emplace_back(false, i++);
    } else {
      order.emplace_back(true, j++);
    }
  }
  std::vector<std::pair<bool, const Segment*>> active;
  for (const auto& [from_q, k] : order) {
    const Segment& s = from_q ? (*q)[k] : p[k];
    std::erase_if(active, [&](const auto& a) { return a.second->max_x < s.min_x - slack; });
    for (const auto& [other_q, t] : active) {
      if (!same && other_q == from_q) continue;
      if (same) {
        const std::size_t lo = std::min(s.index, t->index);
        const std::size_t hi = std::max(s.index, t->index);
        if (hi - lo == 1 || hi - lo + 1 == sides) continue;  // neighbours share a point
      }
      if (std::min(s.a.y, s.b.y) > std::max(t->a.y, t->b.y) + slack ||
          std::min(t->a.y, t->b.y) > std::max(s.a.y, s.b.y) + slack) {
        continue;
      }
      if (crosses(s, *t, slack)) return true;
    }
    active.emplace_back(from_q, &s);
  }
  return false;
}

}  // namespace

double Profile::area() const {
  double a = outer.area;
  for (const auto& h : holes) a -= h.area;
  return a;
}

Result<std::vector<Profile>> extract_profiles(const Sketch& sketch,
                                              const ProductSettings& settings) {
  const auto problems = sketch.validate();
  if (problems.has_errors()) return problems[0];

  const double tol = settings.linear_tolerance_mm;
  // Arcs become polygons for the containment and crossing tests below;
  // their chords stray up to `deflection` from the arcs, so points that
  // close count as touching.
  const double deflection = settings.min_feature_size_mm();
  const double slack = tol + deflection;
  const auto sketch_ref = sketch.sketch_subject();

  std::vector<Curve> open;  // lines and arcs; curve i has ends 2i and 2i + 1
  std::vector<Loop> loops;
  for (const auto& c : profile_curves(sketch)) {
    if (c.kind != EntityKind::Circle) {
      open.push_back(c);
      continue;
    }
    // A circle is a loop on its own.
    Loop loop;
    loop.edges.push_back({c.id, false});
    loop.signed_area = edge_area(c, false);
    sample_edge(c, false, deflection, loop.polygon);
    loops.push_back(std::move(loop));
  }
  auto end_position = [&](std::size_t e) { return open[e / 2].ends[e % 2]; };
  auto end_point_id = [&](std::size_t e) { return open[e / 2].end_ids[e % 2]; };

  // --- joints: curve ends joined by a shared point or within tolerance.
  const std::size_t end_count = open.size() * 2;
  std::vector<std::size_t> parent(end_count);
  std::iota(parent.begin(), parent.end(), std::size_t{0});
  auto root = [&](std::size_t v) {
    while (parent[v] != v) v = parent[v] = parent[parent[v]];
    return v;
  };
  for (std::size_t a = 0; a < end_count; ++a) {
    for (std::size_t b = a + 1; b < end_count; ++b) {
      if (end_point_id(a) == end_point_id(b) ||
          core::distance(end_position(a), end_position(b)) <= tol) {
        parent[root(a)] = root(b);
      }
    }
  }
  std::vector<std::vector<std::size_t>> joint(end_count);
  for (std::size_t e = 0; e < end_count; ++e) joint[root(e)].push_back(e);
  for (const auto& m : joint) {
    if (m.empty() || m.size() == 2) continue;
    std::vector<core::SubjectRef> subjects;
    for (const auto e : m) subjects.push_back(sketch.subject(open[e / 2].id));
    subjects.push_back(sketch_ref);
    if (m.size() == 1) {
      return Diagnostic::error(
          "sketch.profile.open",
          fmt::format("{} has an end that meets no other geometry, so it closes no profile; "
                      "connect it or make it construction geometry",
                      sketch.label(open[m[0] / 2].id)),
          std::move(subjects));
    }
    return Diagnostic::error(
        "sketch.profile.branching",
        fmt::format("{} curve ends meet at one point, so the profile is ambiguous; "
                    "make the extra curves construction geometry",
                    m.size()),
        std::move(subjects));
  }
  // The other curve end at the joint of end `e`.
  auto partner = [&](std::size_t e) {
    const auto& m = joint[root(e)];
    return m[0] == e ? m[1] : m[0];
  };

  // --- walk the loops of lines and arcs.
  std::vector<bool> used(open.size(), false);
  for (std::size_t first = 0; first < open.size(); ++first) {
    if (used[first]) continue;
    Loop loop;
    std::size_t curve = first;
    bool reversed = false;
    while (!used[curve]) {
      used[curve] = true;
      loop.edges.push_back({open[curve].id, reversed});
      loop.signed_area += edge_area(open[curve], reversed);
      sample_edge(open[curve], reversed, deflection, loop.polygon);
      // Leave by the far end and carry on along the curve that meets it.
      const std::size_t next_end = partner(curve * 2 + (reversed ? 0 : 1));
      curve = next_end / 2;
      reversed = next_end % 2 == 1;  // entered at its end, so run it backwards
    }
    loops.push_back(std::move(loop));
  }

  // --- orientation: every loop counter-clockwise for now.
  for (auto& loop : loops) {
    if (std::abs(loop.signed_area) <= tol * tol) {
      return Diagnostic::error("sketch.profile.degenerate", "A closed loop encloses no area",
                               {sketch.subject(loop.edges.front().entity), sketch_ref});
    }
    if (loop.signed_area < 0) {
      std::ranges::reverse(loop.edges);
      for (auto& e : loop.edges) e.reversed = !e.reversed;
      std::ranges::reverse(loop.polygon);
      loop.signed_area = -loop.signed_area;
    }
  }

  // --- a loop may not cross itself (a bowtie).
  std::vector<std::vector<Segment>> sides;
  sides.reserve(loops.size());
  for (auto& loop : loops) {
    for (const auto& p : loop.polygon) loop.box.expand(p);
    sides.push_back(segments_by_x(loop.polygon));
    if (any_crossing(sides.back(), nullptr, slack, loop.polygon.size())) {
      return Diagnostic::error(
          "sketch.profile.self_crossing",
          fmt::format("The loop through {} crosses itself; a profile's outline must not",
                      sketch.label(loop.edges.front().entity)),
          {sketch.subject(loop.edges.front().entity), sketch_ref});
    }
  }

  // --- nesting. contains[i][j]: loop j lies inside loop i.
  const std::size_t n = loops.size();
  auto crossing = [&](std::size_t i, std::size_t j, std::string_view how) {
    return Diagnostic::error(
        "sketch.profile.loops_cross",
        fmt::format("The loop through {} {} the loop through {}",
                    sketch.label(loops[j].edges.front().entity), how,
                    sketch.label(loops[i].edges.front().entity)),
        {sketch.subject(loops[j].edges.front().entity),
         sketch.subject(loops[i].edges.front().entity), sketch_ref});
  };
  std::vector<std::vector<bool>> contains(n, std::vector<bool>(n, false));
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      if (!loops[i].box.intersects(loops[j].box, slack)) continue;  // apart
      // Sides crossing each other (two rectangles in a '+'), even where
      // no corner of either lies inside the other.
      if (any_crossing(sides[i], &sides[j], slack, 0)) return crossing(i, j, "crosses");
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j < n; ++j) {
      if (i == j || !loops[i].box.intersects(loops[j].box, slack)) continue;
      std::size_t in = 0;
      std::size_t out = 0;
      for (const auto& p : loops[j].polygon) {
        if (!loops[i].box.contains(p, slack)) {
          ++out;
          continue;
        }
        if (distance_to_polygon(loops[i].polygon, p) <= slack) continue;  // touching
        if (inside(loops[i].polygon, p)) {
          ++in;
        } else {
          ++out;
        }
      }
      if (in > 0 && out > 0) return crossing(i, j, "crosses");
      if (in == 0 && out == 0) return crossing(i, j, "lies on");  // the same loop twice
      contains[i][j] = in > 0 && loops[i].signed_area > loops[j].signed_area;
    }
  }

  std::vector<std::size_t> depth(n, 0);
  std::vector<std::optional<std::size_t>> parent_loop(n);
  for (std::size_t j = 0; j < n; ++j) {
    for (std::size_t i = 0; i < n; ++i) {
      if (!contains[i][j]) continue;
      ++depth[j];
      if (!parent_loop[j] || loops[i].signed_area < loops[*parent_loop[j]].signed_area) {
        parent_loop[j] = i;
      }
    }
  }

  // Outer loops, largest first (ties keep sketch order), then their holes.
  std::vector<std::size_t> order(n);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
    return loops[a].signed_area > loops[b].signed_area;
  });
  std::vector<Profile> profiles;
  std::vector<std::size_t> profile_of(n, 0);
  for (const auto i : order) {
    if (depth[i] % 2 != 0) continue;
    profile_of[i] = profiles.size();
    profiles.push_back({{loops[i].edges, loops[i].signed_area}, {}});
  }
  for (const auto j : order) {
    if (depth[j] % 2 == 0) continue;
    ProfileLoop hole{loops[j].edges, loops[j].signed_area};
    std::ranges::reverse(hole.edges);  // holes run clockwise
    for (auto& e : hole.edges) e.reversed = !e.reversed;
    profiles[profile_of[*parent_loop[j]]].holes.push_back(std::move(hole));
  }
  return profiles;
}

// --- faces --------------------------------------------------------------

namespace {

kernel::Loop to_kernel_loop(const Sketch& sketch, const ProfileLoop& loop) {
  const SketchPlane& plane = sketch.plane();
  if (loop.edges.size() == 1) {
    if (const auto circle = circle_geometry(sketch, loop.edges.front().entity)) {
      return kernel::Loop::circle(plane.to_model(circle->center), plane.normal(),
                                  circle->radius);
    }
  }
  kernel::Loop out;
  for (const auto& edge : loop.edges) {
    if (const auto line = line_geometry(sketch, edge.entity)) {
      kernel::LineSegment s{plane.to_model(line->start), plane.to_model(line->end)};
      if (edge.reversed) std::swap(s.start, s.end);
      out.segments.emplace_back(s);
    } else if (const auto arc = arc_geometry(sketch, edge.entity)) {
      kernel::ArcSegment s{plane.to_model(arc->start()),
                           plane.to_model(arc->at(0.5 * (arc->start_angle + arc->end_angle))),
                           plane.to_model(arc->end())};
      if (edge.reversed) std::swap(s.start, s.end);
      out.segments.emplace_back(s);
    }
  }
  return out;
}

}  // namespace

Result<kernel::Face> make_face(const Sketch& sketch, const Profile& profile,
                               const ProductSettings& settings) {
  const kernel::Loop outer = to_kernel_loop(sketch, profile.outer);
  std::vector<kernel::Loop> holes;
  for (const auto& h : profile.holes) holes.push_back(to_kernel_loop(sketch, h));
  auto face = kernel::make_face(outer, holes, settings);
  if (!face) {
    Diagnostic d = face.error();
    d.subjects.insert(d.subjects.begin(), sketch.subject(profile.outer.edges.front().entity));
    d.subjects.push_back(sketch.sketch_subject());
    return d;
  }
  return face;
}

Result<std::vector<kernel::Face>> make_faces(const Sketch& sketch,
                                             const ProductSettings& settings) {
  auto profiles = extract_profiles(sketch, settings);
  if (!profiles) return profiles.error();
  std::vector<kernel::Face> faces;
  for (const auto& p : *profiles) {
    auto face = make_face(sketch, p, settings);
    if (!face) return face.error();
    faces.push_back(std::move(face).value());
  }
  return faces;
}

}  // namespace vs::sketch

// Frames (plan, M12): scatter (seeded points in the volume or on a
// connection surface) and patterns (linear, circular, grid).
//
// A frame is an FP32 rigid placement: columns x, y, z (orthonormal) and a
// translation. Patterns transform every input frame; with no frames
// connected they start from one frame at the origin, so a pattern alone
// is a set of points.

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/format.h>

#include "node_helpers.hpp"
#include "vs/proc/field.hpp"

namespace vs::proc::detail {

namespace {

// The most frames one node may make.
constexpr double kMaxFrames = 100000;

core::ftransform frame_at(const dvec3& origin, const dvec3& z_dir) {
  const dvec3 z = normalize(z_dir);
  const dvec3 helper = std::abs(z.x) < 0.9 ? dvec3{1, 0, 0} : dvec3{0, 1, 0};
  const dvec3 x = normalize(helper - z * dot(helper, z));  // world x where it can be
  const dvec3 y = cross(z, x);
  core::ftransform t;
  t.linear.cols = {core::to_fp32(x), core::to_fp32(y), core::to_fp32(z)};
  t.translation = core::to_fp32(origin);
  return t;
}

// The surface's normal at `p` on its patch: the plane's, or for a curved
// surface the direction from its axis to `p` (H3; it was the axis).
dvec3 surface_normal(const ConnectionInput& c, const dvec3& p) {
  if (!c.radius) return c.normal;
  const dvec3 axis = normalize(c.normal);
  const dvec3 d = p - c.origin;
  const dvec3 radial = d - axis * dot(d, axis);
  return length(radial) > 0.0 ? radial : c.x_axis;
}

// The input frames, or one frame at the origin.
std::vector<core::ftransform> input_frames(NodeContext& ctx) {
  if (ctx.has("frames")) return ctx.frames("frames")->frames;
  return {core::ftransform{}};
}

bool check_total(NodeContext& ctx, std::vector<std::string> ports, double total) {
  if (total > kMaxFrames) {
    ctx.infeasible(std::move(ports), fmt::format("that makes {:.0f} frames, more than the {:.0f} "
                                                 "allowed",
                                                 total, kMaxFrames));
    return false;
  }
  return true;
}

void set_frames(NodeContext& ctx, std::vector<core::ftransform> frames) {
  const auto count = static_cast<double>(frames.size());
  auto set = std::make_shared<FrameSet>();
  set->frames = std::move(frames);
  ctx.set("frames", Frames(std::move(set)));
  ctx.set("count", count);
}

// A rigid transform in FP64 applied to an FP32 frame.
core::ftransform apply(const core::dtransform& t, const core::ftransform& f) {
  return core::to_fp32(t * core::to_fp64(f));
}

// Rotation by `angle` about the unit `axis` through `centre` (Rodrigues,
// with sin_det and cos_det so it is the same everywhere).
core::dtransform rotation(const dvec3& centre, const dvec3& axis, double angle) {
  const double c = cos_det(angle);
  const double s = sin_det(angle);
  const double t = 1.0 - c;
  const dvec3 a = axis;
  core::dtransform r;
  r.linear.cols = {dvec3{t * a.x * a.x + c, t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y},
                   dvec3{t * a.x * a.y - s * a.z, t * a.y * a.y + c, t * a.y * a.z + s * a.x},
                   dvec3{t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c}};
  r.translation = centre - r.linear * centre;
  return r;
}

// Dart throwing with a minimum spacing, through a hash grid of cells
// `spacing` wide. Deterministic: candidates are tried in index order.
class SpacingGrid {
 public:
  explicit SpacingGrid(double spacing) : spacing_(spacing) {}

  bool accepts(const dvec3& p) const {
    if (spacing_ <= 0.0) return true;
    const auto c = cell(p);
    for (std::int64_t dz = -1; dz <= 1; ++dz) {
      for (std::int64_t dy = -1; dy <= 1; ++dy) {
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
          const auto it = points_.find({c[0] + dx, c[1] + dy, c[2] + dz});
          if (it == points_.end()) continue;
          for (const dvec3& q : it->second) {
            if (length(p - q) < spacing_) return false;
          }
        }
      }
    }
    return true;
  }

  void add(const dvec3& p) {
    if (spacing_ > 0.0) points_[cell(p)].push_back(p);
  }

 private:
  using Key = std::array<std::int64_t, 3>;
  Key cell(const dvec3& p) const {
    return {static_cast<std::int64_t>(std::floor(p.x / spacing_)),
            static_cast<std::int64_t>(std::floor(p.y / spacing_)),
            static_cast<std::int64_t>(std::floor(p.z / spacing_))};
  }
  double spacing_;
  std::map<Key, std::vector<dvec3>> points_;
};

// Candidates tried per point asked for before a scatter gives up.
constexpr std::uint64_t kAttemptsPerPoint = 64;

bool check_scatter(NodeContext& ctx, double count, double spacing) {
  if (!check_count(ctx, "count", count, 0, kMaxFrames)) return false;
  if (!(spacing >= 0.0) || !std::isfinite(spacing)) {
    ctx.infeasible({"spacing"}, "the spacing can't be negative");
    return false;
  }
  return true;
}

void too_few(NodeContext& ctx, std::size_t found, double count, std::vector<std::string> ports) {
  ctx.infeasible(std::move(ports),
                 fmt::format("only {} of {:.0f} points fit after {} tries each; ask for fewer, "
                             "or a smaller spacing or margin",
                             found, count, kAttemptsPerPoint));
}

}  // namespace

void add_frame_nodes(NodeRegistry& r) {
  r.add(make("frames.point", "frames", "One frame at `position`, its z axis along `direction`",
             {in("position", P::Vector, vec(0, 0, 0)), in("direction", P::Vector, vec(0, 0, 1))},
             {out("frames", P::Frames), out("count", P::Scalar)}, [](NodeContext& ctx) {
               if (!check_direction(ctx, "direction", ctx.vector("direction"))) return;
               set_frames(ctx, {frame_at(ctx.vector("position"), ctx.vector("direction"))});
             }));

  r.add(make("frames.count", "frames", "How many frames there are", {in("frames", P::Frames)},
             {out("count", P::Scalar)}, [](NodeContext& ctx) {
               ctx.set("count", static_cast<double>(ctx.frames("frames")->frames.size()));
             }));

  r.add(make("frames.merge", "frames", "The frames of `a`, then those of `b`",
             {in("a", P::Frames), in("b", P::Frames)},
             {out("frames", P::Frames), out("count", P::Scalar)}, [](NodeContext& ctx) {
               std::vector<core::ftransform> all = ctx.frames("a")->frames;
               const auto& b = ctx.frames("b")->frames;
               if (!check_total(ctx, {"a", "b"}, static_cast<double>(all.size() + b.size()))) return;
               all.insert(all.end(), b.begin(), b.end());
               set_frames(ctx, std::move(all));
             }));

  r.add(make(
      "scatter.volume", "scatter",
      "`count` seeded random points inside the boundary, at least `margin` inside it and "
      "`spacing` apart (0: no minimum). The node's seed decides them, so they stay put until "
      "the volume's seed changes",
      {in("count", P::Scalar, num(50)), in("margin", P::Scalar, num(0)),
       in("spacing", P::Scalar, num(0))},
      {out("frames", P::Frames), out("count", P::Scalar)},
      [](NodeContext& ctx) {
        const double count = ctx.scalar("count");
        const double margin = ctx.scalar("margin");
        const double spacing = ctx.scalar("spacing");
        if (!check_scatter(ctx, count, spacing)) return;
        if (!(margin >= 0.0) || !std::isfinite(margin)) {
          ctx.infeasible({"margin"}, "the margin can't be negative");
          return;
        }
        const GraphInputs& gi = ctx.inputs();
        if (gi.boundary == nullptr || gi.boundary->empty()) {
          ctx.fail("the volume has no boundary to scatter in");
          return;
        }
        const dvec3 lo = gi.bounds.min;
        const dvec3 size = gi.bounds.size();
        const auto wanted = static_cast<std::size_t>(count);
        const std::uint64_t attempts = kAttemptsPerPoint * static_cast<std::uint64_t>(wanted);
        SpacingGrid grid(spacing);
        std::vector<core::ftransform> frames;
        for (std::uint64_t i = 0; i < attempts && frames.size() < wanted; ++i) {
          if (i % 4096 == 0 && ctx.cancelled()) return;
          const dvec3 p{lo.x + size.x * uniform(ctx.seed(), 3 * i),
                        lo.y + size.y * uniform(ctx.seed(), 3 * i + 1),
                        lo.z + size.z * uniform(ctx.seed(), 3 * i + 2)};
          if (gi.boundary->signed_distance(p) > -margin) continue;
          if (!grid.accepts(p)) continue;
          grid.add(p);
          core::ftransform f;  // axis-aligned
          f.translation = core::to_fp32(p);
          frames.push_back(f);
        }
        if (frames.size() < wanted) {
          too_few(ctx, frames.size(), count, {"count", "margin", "spacing"});
          return;
        }
        set_frames(ctx, std::move(frames));
      },
      {}, false));

  r.add(make(
      "scatter.surface", "scatter",
      "`count` seeded random points on a connection surface's patch, `spacing` apart (0: no "
      "minimum), each frame's z along the surface's normal there (a curved surface's points "
      "away from its axis)",
      {in("count", P::Scalar, num(20)), in("spacing", P::Scalar, num(0))},
      {out("frames", P::Frames), out("count", P::Scalar)},
      [](NodeContext& ctx) {
        const ConnectionInput* c = surface_of(ctx);
        if (c == nullptr) return;
        const double count = ctx.scalar("count");
        const double spacing = ctx.scalar("spacing");
        if (!check_scatter(ctx, count, spacing)) return;
        const TriangleDistance& patch = *c->patch;
        // Cumulative areas, so a uniform number picks a triangle by area.
        std::vector<double> cumulative;
        double total = 0.0;
        const auto& pos = patch.positions();
        for (std::size_t t = 0; t < patch.triangle_count(); ++t) {
          const auto v = patch.triangle(t);
          total += 0.5 * length(cross(pos[v[1]] - pos[v[0]], pos[v[2]] - pos[v[0]]));
          cumulative.push_back(total);
        }
        const auto wanted = static_cast<std::size_t>(count);
        if (wanted > 0 && !(total > 0.0)) {
          ctx.fail("the surface's patch has no area");
          return;
        }
        const std::uint64_t attempts = kAttemptsPerPoint * static_cast<std::uint64_t>(wanted);
        SpacingGrid grid(spacing);
        std::vector<core::ftransform> frames;
        for (std::uint64_t i = 0; i < attempts && frames.size() < wanted; ++i) {
          if (i % 4096 == 0 && ctx.cancelled()) return;
          const double pick = uniform(ctx.seed(), 3 * i) * total;
          const auto it = std::upper_bound(cumulative.begin(), cumulative.end(), pick);
          const auto t = static_cast<std::size_t>(
              std::min<std::ptrdiff_t>(it - cumulative.begin(),
                                       static_cast<std::ptrdiff_t>(cumulative.size()) - 1));
          double a = uniform(ctx.seed(), 3 * i + 1);
          double b = uniform(ctx.seed(), 3 * i + 2);
          if (a + b > 1.0) {
            a = 1.0 - a;
            b = 1.0 - b;
          }
          const auto v = patch.triangle(t);
          const dvec3 p = pos[v[0]] + (pos[v[1]] - pos[v[0]]) * a + (pos[v[2]] - pos[v[0]]) * b;
          if (!grid.accepts(p)) continue;
          grid.add(p);
          frames.push_back(frame_at(p, surface_normal(*c, p)));
        }
        if (frames.size() < wanted) {
          too_few(ctx, frames.size(), count, {"count", "spacing"});
          return;
        }
        set_frames(ctx, std::move(frames));
      },
      {{"surface", PropertyKind::Surface, std::nullopt, {}, "the surface's id"}}, false));

  r.add(make("pattern.linear", "pattern",
             "`count` copies of each frame, each `step` further on (the first stays put)",
             {optional_in("frames", P::Frames, "frames to repeat (none: one at the origin)"),
              in("count", P::Scalar, num(3)), in("step", P::Vector, vec(10, 0, 0))},
             {out("frames", P::Frames), out("count", P::Scalar)}, [](NodeContext& ctx) {
               const double count = ctx.scalar("count");
               if (!check_count(ctx, "count", count, 1, kMaxFrames)) return;
               const auto base = input_frames(ctx);
               if (!check_total(ctx, {"count"}, count * static_cast<double>(base.size()))) return;
               const dvec3 step = ctx.vector("step");
               std::vector<core::ftransform> frames;
               for (const auto& f : base) {
                 for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
                   core::dtransform t;
                   t.translation = step * static_cast<double>(i);
                   frames.push_back(apply(t, f));
                 }
               }
               set_frames(ctx, std::move(frames));
             }));

  r.add(make("pattern.circular", "pattern",
             "`count` copies of each frame turned about `axis` through `centre`, spread over "
             "`angle` radians (a full turn spaces them evenly; less puts the last at `angle`)",
             {optional_in("frames", P::Frames, "frames to repeat (none: one at the origin)"),
              in("count", P::Scalar, num(6)), in("centre", P::Vector, vec(0, 0, 0)),
              in("axis", P::Vector, vec(0, 0, 1)),
              in("angle", P::Scalar, num(2.0 * std::numbers::pi))},
             {out("frames", P::Frames), out("count", P::Scalar)}, [](NodeContext& ctx) {
               const double count = ctx.scalar("count");
               const double angle = ctx.scalar("angle");
               if (!check_count(ctx, "count", count, 1, kMaxFrames)) return;
               if (!check_direction(ctx, "axis", ctx.vector("axis"))) return;
               if (!std::isfinite(angle)) {
                 ctx.infeasible({"angle"}, "the angle must be finite");
                 return;
               }
               const auto base = input_frames(ctx);
               if (!check_total(ctx, {"count"}, count * static_cast<double>(base.size()))) return;
               const bool full = std::abs(std::abs(angle) - 2.0 * std::numbers::pi) < 1e-9;
               const double step = count <= 1.0 ? 0.0 : angle / (full ? count : count - 1.0);
               const dvec3 axis = normalize(ctx.vector("axis"));
               std::vector<core::ftransform> frames;
               for (const auto& f : base) {
                 for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
                   frames.push_back(
                       apply(rotation(ctx.vector("centre"), axis, step * static_cast<double>(i)), f));
                 }
               }
               set_frames(ctx, std::move(frames));
             }));

  r.add(make("pattern.grid", "pattern",
             "A grid of copies of each frame: `counts` (whole numbers) along x, y and z, "
             "`spacing` apart",
             {optional_in("frames", P::Frames, "frames to repeat (none: one at the origin)"),
              in("counts", P::Vector, vec(3, 3, 1)), in("spacing", P::Vector, vec(10, 10, 10))},
             {out("frames", P::Frames), out("count", P::Scalar)}, [](NodeContext& ctx) {
               const dvec3 n = ctx.vector("counts");
               for (const double c : {n.x, n.y, n.z}) {
                 if (!check_count(ctx, "counts", c, 1, kMaxFrames)) return;
               }
               const auto base = input_frames(ctx);
               if (!check_total(ctx, {"counts"}, n.x * n.y * n.z * static_cast<double>(base.size()))) {
                 return;
               }
               const dvec3 s = ctx.vector("spacing");
               std::vector<core::ftransform> frames;
               for (const auto& f : base) {
                 for (std::size_t k = 0; k < static_cast<std::size_t>(n.z); ++k) {
                   for (std::size_t j = 0; j < static_cast<std::size_t>(n.y); ++j) {
                     for (std::size_t i = 0; i < static_cast<std::size_t>(n.x); ++i) {
                       core::dtransform t;
                       t.translation = {s.x * static_cast<double>(i), s.y * static_cast<double>(j),
                                        s.z * static_cast<double>(k)};
                       frames.push_back(apply(t, f));
                     }
                   }
                 }
               }
               set_frames(ctx, std::move(frames));
             }));
}

}  // namespace vs::proc::detail

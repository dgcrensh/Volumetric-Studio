#include "vs/validate/containment.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <cmath>

#include <fmt/format.h>

#include "vs/kernel/operations.hpp"
#include "vs/kernel/queries.hpp"

namespace vs::validate {

using core::dbox3;
using core::Diagnostic;
using core::dvec3;

namespace {

std::string mm(double v) { return fmt::format("{:.6g}", v); }

std::string box_text(const dbox3& b) {
  return fmt::format("({}, {}, {}) to ({}, {}, {})", mm(b.min.x), mm(b.min.y), mm(b.min.z),
                     mm(b.max.x), mm(b.max.y), mm(b.max.z));
}

ContainmentResult undecided(const core::SubjectRef& volume, const std::string& why) {
  ContainmentResult r;
  r.contained = false;
  r.diagnostics.add(Diagnostic::error(
      "validate.undecided",
      fmt::format("Couldn't check that the contents of '{}' stay inside its boundary: {}",
                  volume.label, why),
      {volume}));
  return r;
}

void report(ContainmentResult& r, const core::SubjectRef& volume, std::string detail) {
  r.contained = false;
  r.diagnostics.add(Diagnostic::error(
      "validate.containment",
      fmt::format("The contents of '{}' reach {} mm outside its boundary{}, from {}; "
                  "keep them inside the boundary",
                  volume.label, mm(r.reach), detail, box_text(r.region)),
      {volume}));
}

// FP32 rounding of a point: two units in the last place of its largest
// coordinate.
double fp32_slack(const dvec3& p) {
  const double m = std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)});
  return std::ldexp(m, -22);
}

}  // namespace

ContainmentResult check_containment(const kernel::Solid& boundary, const kernel::Solid& contents,
                                    const core::SubjectRef& volume,
                                    const core::ProductSettings& settings) {
  if (contents.is_empty()) return {};
  if (boundary.is_empty()) return undecided(volume, "the boundary is empty");
  const double tol = settings.linear_tolerance_mm;
  auto outside = kernel::cut(contents, boundary, {tol});
  if (!outside) {
    if (outside.error().code == "kernel.boolean.empty_result") return {};  // all inside
    return undecided(volume, outside.error().message);
  }
  if (outside->is_empty()) return {};
  const auto mass = kernel::mass_properties(*outside);
  if (!mass || mass->volume <= 0.0) return {};

  ContainmentResult r;
  r.region = kernel::bounding_box(*outside);
  r.outside_volume = mass->volume;
  for (const dvec3& p : kernel::vertex_points(*outside)) {
    auto d = kernel::distance_to(boundary, p);
    if (!d) return undecided(volume, d.error().message);
    r.reach = std::max(r.reach, *d);
  }
  if (r.reach <= tol) return {};  // a sliver along the boundary: touching
  report(r, volume, fmt::format(" ({} mm³)", mm(r.outside_volume)));
  return r;
}

namespace {

struct Sample {
  dvec3 p;
  double slack;
};

// Points the octree leaves to the classifier, in sample order.
constexpr std::size_t kLeafSamples = 48;
constexpr int kMaxDepth = 16;
// Samples per classification chunk: fixed, so results don't depend on how
// the chunks are run.
constexpr std::size_t kChunk = 2048;

// Certifies whole groups of samples as inside: a node's samples all lie
// within its box, and a ball round an inside point that doesn't reach the
// boundary's surface is inside. What can't be certified is left in `pending`.
class Certifier {
 public:
  Certifier(const kernel::Solid& boundary, const core::ProductSettings& settings,
            const std::vector<Sample>& samples)
      : classifier_(boundary, settings), faces_(kernel::faces(boundary)), samples_(samples) {}

  void run(std::vector<std::uint32_t> group, int depth, std::vector<std::uint32_t>& pending) {
    if (group.size() <= kLeafSamples || depth >= kMaxDepth || failed_) {
      pending.insert(pending.end(), group.begin(), group.end());
      return;
    }
    dbox3 box = dbox3::empty();
    for (const auto i : group) box.expand(samples_[i].p);
    const dvec3 c = (box.min + box.max) * 0.5;
    const double r = 0.5 * length(box.max - box.min);
    if (classifier_.classify(c) == kernel::PointClass::Inside) {
      double d = std::numeric_limits<double>::infinity();
      for (const auto& f : faces_) {
        auto fd = kernel::distance_to(f, c);
        if (!fd) {
          failed_ = true;  // leave it all to the classifier
          d = 0.0;
          break;
        }
        d = std::min(d, *fd);
      }
      if (d >= r) return;  // every sample in the group is inside
    }
    std::array<std::vector<std::uint32_t>, 8> children;
    for (const auto i : group) {
      const dvec3& p = samples_[i].p;
      const std::size_t k = (p.x > c.x ? 1u : 0u) | (p.y > c.y ? 2u : 0u) | (p.z > c.z ? 4u : 0u);
      children[k].push_back(i);
    }
    group.clear();
    group.shrink_to_fit();
    for (auto& child : children) {
      if (child.empty()) continue;
      if (child.size() == samples_.size()) {  // can't split (all at one point)
        pending.insert(pending.end(), child.begin(), child.end());
        continue;
      }
      run(std::move(child), depth + 1, pending);
    }
  }

 private:
  kernel::SolidClassifier classifier_;
  std::vector<kernel::Face> faces_;
  const std::vector<Sample>& samples_;
  bool failed_ = false;
};

// A boundary that is one convex polyhedron (every face planar, every vertex
// behind every face's plane): a point behind every plane is inside, with
// no kernel call. Box boundaries, the commonest, are such.
std::optional<std::vector<kernel::Plane>> convex_planes(const kernel::Solid& boundary, double tol) {
  if (boundary.solid_count() != 1) return std::nullopt;
  std::vector<kernel::Plane> planes;
  for (const auto& f : kernel::faces(boundary)) {
    auto plane = kernel::face_plane(f);
    if (!plane) return std::nullopt;
    plane->normal = normalize(plane->normal);
    planes.push_back(*plane);
  }
  if (planes.size() < 4) return std::nullopt;
  for (const dvec3& v : kernel::vertex_points(boundary)) {
    for (const auto& pl : planes) {
      if (dot(v - pl.origin, pl.normal) > tol) return std::nullopt;
    }
  }
  return planes;
}

struct ChunkResult {
  std::size_t outside = 0;
  dbox3 region = dbox3::empty();
  double reach = 0.0;
  bool failed = false;
  std::string why;
};

}  // namespace

ContainmentResult check_containment(const kernel::Solid& boundary, const MeshView& mesh,
                                    const core::SubjectRef& volume,
                                    const core::ProductSettings& settings,
                                    const ParallelFor& parallel) {
  if (mesh.positions.empty()) return {};
  if (boundary.is_empty()) return undecided(volume, "the boundary is empty");
  if (mesh.indices.size() % 3 != 0) {
    return undecided(volume, "the mesh's index count isn't a multiple of three");
  }
  if (!mesh.centroid_slack.empty() && mesh.centroid_slack.size() != mesh.indices.size() / 3) {
    return undecided(volume, "the mesh's centroid slack doesn't have one value per triangle");
  }
  for (const auto i : mesh.indices) {
    if (i >= mesh.positions.size()) {
      return undecided(volume, fmt::format("the mesh refers to vertex {}, but has only {}", i,
                                           mesh.positions.size()));
    }
  }

  // Every vertex, then every triangle's centroid.
  std::vector<Sample> samples;
  samples.reserve(mesh.positions.size() + mesh.indices.size() / 3);
  for (const auto& v : mesh.positions) {
    const dvec3 p = core::to_fp64(v);
    samples.push_back({p, fp32_slack(p)});
  }
  for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const dvec3 a = core::to_fp64(mesh.positions[mesh.indices[t]]);
    const dvec3 b = core::to_fp64(mesh.positions[mesh.indices[t + 1]]);
    const dvec3 c = core::to_fp64(mesh.positions[mesh.indices[t + 2]]);
    const double extra = mesh.centroid_slack.empty() ? 0.0 : mesh.centroid_slack[t / 3];
    samples.push_back(
        {(a + b + c) / 3.0, std::max({fp32_slack(a), fp32_slack(b), fp32_slack(c)}) + extra});
  }

  const double tol = settings.linear_tolerance_mm;
  const dbox3 box = kernel::bounding_box(boundary);

  // Certify what lies deep inside in bulk; classify the rest one by one.
  std::vector<std::uint32_t> pending;
  if (const auto planes = convex_planes(boundary, tol)) {
    // Behind every plane: inside. Anything else goes to the classifier.
    for (std::uint32_t i = 0; i < samples.size(); ++i) {
      for (const auto& pl : *planes) {
        if (dot(samples[i].p - pl.origin, pl.normal) > 0.0) {
          pending.push_back(i);
          break;
        }
      }
    }
  } else {
    std::vector<std::uint32_t> all(samples.size());
    for (std::uint32_t i = 0; i < all.size(); ++i) all[i] = i;
    Certifier certifier(boundary, settings, samples);
    certifier.run(std::move(all), 0, pending);
    std::sort(pending.begin(), pending.end());
  }

  const std::size_t chunks = (pending.size() + kChunk - 1) / kChunk;
  std::vector<ChunkResult> results(chunks);
  const auto chunk = [&](std::size_t k) {
    ChunkResult& out = results[k];
    const kernel::SolidClassifier classifier(boundary, settings);
    const std::size_t end = std::min(pending.size(), (k + 1) * kChunk);
    for (std::size_t j = k * kChunk; j < end && !out.failed; ++j) {
      const Sample& s = samples[pending[j]];
      const bool surely_out = !box.contains(s.p, tol + s.slack);
      if (!surely_out && classifier.classify(s.p) != kernel::PointClass::Outside) continue;
      auto d = kernel::distance_to(boundary, s.p);
      if (!d) {
        out.failed = true;
        out.why = d.error().message;
        continue;
      }
      if (*d <= tol + s.slack) continue;
      ++out.outside;
      out.region.expand(s.p);
      out.reach = std::max(out.reach, *d);
    }
  };
  if (parallel && chunks > 1) {
    parallel(chunks, chunk);
  } else {
    for (std::size_t k = 0; k < chunks; ++k) chunk(k);
  }

  ContainmentResult r;
  r.region = dbox3::empty();
  for (const ChunkResult& c : results) {
    if (c.failed) return undecided(volume, c.why);
    r.outside_points += c.outside;
    if (c.outside > 0) {
      r.region.expand(c.region.min);
      r.region.expand(c.region.max);
    }
    r.reach = std::max(r.reach, c.reach);
  }
  if (r.outside_points == 0) return {};
  report(r, volume, fmt::format(" ({} of its vertices and triangle centres)", r.outside_points));
  return r;
}

}  // namespace vs::validate

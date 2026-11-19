#include "vs/validate/containment.hpp"

#include <algorithm>
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

ContainmentResult check_containment(const kernel::Solid& boundary, const MeshView& mesh,
                                    const core::SubjectRef& volume,
                                    const core::ProductSettings& settings) {
  if (mesh.positions.empty()) return {};
  if (boundary.is_empty()) return undecided(volume, "the boundary is empty");
  if (mesh.indices.size() % 3 != 0) {
    return undecided(volume, "the mesh's index count isn't a multiple of three");
  }
  for (const auto i : mesh.indices) {
    if (i >= mesh.positions.size()) {
      return undecided(volume, fmt::format("the mesh refers to vertex {}, but has only {}", i,
                                           mesh.positions.size()));
    }
  }

  const double tol = settings.linear_tolerance_mm;
  const kernel::SolidClassifier classifier(boundary, settings);
  const dbox3 box = kernel::bounding_box(boundary);
  ContainmentResult r;
  r.region = dbox3::empty();
  bool failed = false;
  std::string why;

  auto test = [&](const dvec3& p, double slack) {
    if (failed) return;
    const bool surely_out = !box.contains(p, tol + slack);
    if (!surely_out && classifier.classify(p) != kernel::PointClass::Outside) return;
    auto d = kernel::distance_to(boundary, p);
    if (!d) {
      failed = true;
      why = d.error().message;
      return;
    }
    if (*d <= tol + slack) return;
    ++r.outside_points;
    r.region.expand(p);
    r.reach = std::max(r.reach, *d);
  };

  for (const auto& v : mesh.positions) {
    const dvec3 p = core::to_fp64(v);
    test(p, fp32_slack(p));
  }
  for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const dvec3 a = core::to_fp64(mesh.positions[mesh.indices[t]]);
    const dvec3 b = core::to_fp64(mesh.positions[mesh.indices[t + 1]]);
    const dvec3 c = core::to_fp64(mesh.positions[mesh.indices[t + 2]]);
    const dvec3 centroid = (a + b + c) / 3.0;
    test(centroid, std::max({fp32_slack(a), fp32_slack(b), fp32_slack(c)}));
  }
  if (failed) return undecided(volume, why);
  if (r.outside_points == 0) return {};
  report(r, volume, fmt::format(" ({} of its vertices and triangle centres)", r.outside_points));
  return r;
}

}  // namespace vs::validate

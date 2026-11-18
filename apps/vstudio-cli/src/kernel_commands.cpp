#include "kernel_commands.hpp"

#include <filesystem>
#include <string>

#include <fmt/format.h>

#include "vs/core/settings.hpp"
#include "vs/kernel/kernel.hpp"

namespace vs::cli {

int run_kernel_demo(std::span<const std::string_view> args, std::ostream& out,
                    std::ostream& err) {
  if (args.size() > 1) {
    err << "usage: vstudio-cli kernel-demo [<out.step>]\n";
    return 2;
  }
  const std::filesystem::path file =
      args.empty() ? std::filesystem::path("kernel-demo.step")
                   : std::filesystem::path(std::u8string(args[0].begin(), args[0].end()));
  const auto settings = core::ProductSettings::defaults();

  const auto part = kernel::make_demo_part(settings);
  if (!part) {
    err << part.error().to_string() << "\n";
    return 1;
  }
  const auto mass = kernel::mass_properties(*part);
  if (!mass) {
    err << mass.error().to_string() << "\n";
    return 1;
  }
  const auto box = kernel::bounding_box(*part);
  const auto audit = kernel::audit_tolerances(*part, settings);
  const auto problems = kernel::check_validity(*part);

  out << fmt::format("demo part: {} faces, {} edges, {} vertices\n", kernel::faces(*part).size(),
             kernel::edges(*part).size(), kernel::vertex_count(*part));
  out << fmt::format("  volume          {:.6f} mm³ (analytic {:.6f})\n", mass->volume,
             kernel::demo_part_volume());
  out << fmt::format("  centre of mass  ({:.6f}, {:.6f}, {:.6f}) mm\n", mass->center_of_mass.x,
             mass->center_of_mass.y, mass->center_of_mass.z);
  out << fmt::format("  bounding box    ({}, {}, {}) to ({}, {}, {}) mm\n", box.min.x, box.min.y,
             box.min.z, box.max.x, box.max.y, box.max.z);
  out << fmt::format("  valid           {}\n", problems.empty() ? "yes" : "no");
  out << fmt::format("  max tolerance   {:.3g} mm (limit {:.3g} mm)\n", audit.max_tolerance(),
             audit.limit_mm);
  for (const auto& d : problems) err << d.to_string() << "\n";
  for (const auto& d : audit.diagnostics()) err << d.to_string() << "\n";

  const auto written = kernel::write_step(*part, file);
  for (const auto& d : written) err << d.to_string() << "\n";
  if (!written.empty()) return 1;
  out << fmt::format("wrote {}\n", file.string());
  return problems.empty() ? 0 : 1;
}

int run_tolerance_spike(std::span<const std::string_view> args, std::ostream& out,
                        std::ostream& err) {
  if (!args.empty()) {
    err << "usage: vstudio-cli tolerance-spike\n";
    return 2;
  }
  const auto settings = core::ProductSettings::defaults();
  out << fmt::format("working space ±{} mm, linear tolerance {:g} mm\n\n",
             settings.working_half_extent_mm, settings.linear_tolerance_mm);
  out << fmt::format("{:<32} {:>5} {:>6} {:>12} {:>10} {:>10} {:>10} {:>9}\n", "case", "valid",
             "solids", "vol rel err", "max vtx", "max edge", "max face", "over lim");
  bool all_ok = true;
  for (const auto& c : kernel::run_tolerance_spike(settings)) {
    if (!c.built) {
      all_ok = false;
      out << fmt::format("{:<32} FAILED: {}\n", c.name,
                 c.diagnostics.empty() ? std::string("?") : c.diagnostics[0].to_string());
      continue;
    }
    all_ok = all_ok && c.valid;
    out << fmt::format("{:<32} {:>5} {:>6} {:>12.2e} {:>10.2e} {:>10.2e} {:>10.2e} {:>9}\n", c.name,
               c.valid ? "yes" : "NO", c.solids, c.volume_relative_error(), c.audit.vertices.max,
               c.audit.edges.max, c.audit.faces.max, c.audit.exceeding());
  }
  return all_ok ? 0 : 1;
}

}  // namespace vs::cli

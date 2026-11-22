#include "memory.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace vs::cli {

namespace {

[[maybe_unused]] std::optional<double> status_mb(std::string_view field) {
#if defined(__linux__)
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.starts_with(field)) {
      std::istringstream in(line.substr(field.size()));
      double kb = 0;
      if (in >> kb) return kb / 1024.0;
    }
  }
#else
  (void)field;
#endif
  return std::nullopt;
}

}  // namespace

std::optional<double> peak_rss_mb() { return status_mb("VmHWM:"); }

std::optional<double> current_rss_mb() { return status_mb("VmRSS:"); }

void reset_peak_rss() {
#if defined(__linux__)
  std::ofstream("/proc/self/clear_refs") << "5";
#endif
}

}  // namespace vs::cli

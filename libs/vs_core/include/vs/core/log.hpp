#pragma once

// Logging, on top of spdlog. Every module logs through vs::core::logger(),
// which writes to stderr. Use fmt-style format strings:
//
//   vs::core::logger().info("Rebuilt {} volumes in {} ms", count, ms);
//
// Logging is for developers. Anything the user must act on is a Diagnostic.

#include <optional>
#include <string_view>

#include <spdlog/spdlog.h>

namespace vs::core {

enum class LogLevel {
  Trace,
  Debug,
  Info,
  Warning,
  Error,
  Off,
};

// The shared studio logger. Created on first use with level Info.
spdlog::logger& logger();

void set_log_level(LogLevel level);
[[nodiscard]] LogLevel log_level();

// Accepts "trace", "debug", "info", "warning" (or "warn"), "error", "off".
std::optional<LogLevel> parse_log_level(std::string_view text);

}  // namespace vs::core

#include "vs/core/log.hpp"

#include <memory>
#include <mutex>

#include <spdlog/sinks/stdout_color_sinks.h>

namespace vs::core {

namespace {

spdlog::level::level_enum to_spdlog(LogLevel level) {
  switch (level) {
    case LogLevel::Trace: return spdlog::level::trace;
    case LogLevel::Debug: return spdlog::level::debug;
    case LogLevel::Info: return spdlog::level::info;
    case LogLevel::Warning: return spdlog::level::warn;
    case LogLevel::Error: return spdlog::level::err;
    case LogLevel::Off: return spdlog::level::off;
  }
  return spdlog::level::info;
}

LogLevel from_spdlog(spdlog::level::level_enum level) {
  switch (level) {
    case spdlog::level::trace: return LogLevel::Trace;
    case spdlog::level::debug: return LogLevel::Debug;
    case spdlog::level::info: return LogLevel::Info;
    case spdlog::level::warn: return LogLevel::Warning;
    case spdlog::level::err:
    case spdlog::level::critical: return LogLevel::Error;
    default: return LogLevel::Off;
  }
}

std::shared_ptr<spdlog::logger> make_logger() {
  auto log = spdlog::stderr_color_mt("vs");
  log->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
  log->set_level(spdlog::level::info);
  return log;
}

}  // namespace

spdlog::logger& logger() {
  static const std::shared_ptr<spdlog::logger> instance = make_logger();
  return *instance;
}

void set_log_level(LogLevel level) { logger().set_level(to_spdlog(level)); }

LogLevel log_level() { return from_spdlog(logger().level()); }

std::optional<LogLevel> parse_log_level(std::string_view text) {
  if (text == "trace") return LogLevel::Trace;
  if (text == "debug") return LogLevel::Debug;
  if (text == "info") return LogLevel::Info;
  if (text == "warning" || text == "warn") return LogLevel::Warning;
  if (text == "error") return LogLevel::Error;
  if (text == "off") return LogLevel::Off;
  return std::nullopt;
}

}  // namespace vs::core

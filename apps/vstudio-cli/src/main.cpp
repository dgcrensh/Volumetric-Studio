// vstudio-cli: command-line driver for Volumetric CAD Studio.

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "kernel_commands.hpp"
#include "sketch_commands.hpp"
#include "selftest.hpp"
#include "vs/core/log.hpp"
#include "vs/core/version.hpp"

namespace {

constexpr std::string_view kUsage = R"(usage: vstudio-cli [options] <command> [args]

Commands:
  selftest          Run built-in smoke checks of the linked libraries
  kernel-demo [f]   Build the sample part and write it as STEP (default: kernel-demo.step)
  tolerance-spike   Run the M1 kernel tolerance spike and print its measurements
  sketch-solve <in.json> [out.json]
                    Solve a sketch file, report it and its profiles, optionally write it
  version           Print the version (same as --version)
  help              Print this message

Options:
  --version         Print the version and exit
  --log-level <l>   trace, debug, info, warning, error or off (default: warning)
  -h, --help        Print this message and exit
)";

void print_version(std::ostream& out) {
  out << "vstudio-cli " << vs::core::version_string() << " (" << vs::core::git_revision()
      << ")\n";
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);

  vs::core::set_log_level(vs::core::LogLevel::Warning);

  std::string_view command;
  std::vector<std::string_view> rest;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto arg = args[i];
    if (!command.empty()) {
      rest.push_back(arg);
    } else if (arg == "--version") {
      print_version(std::cout);
      return 0;
    } else if (arg == "-h" || arg == "--help") {
      std::cout << kUsage;
      return 0;
    } else if (arg == "--log-level") {
      if (i + 1 >= args.size()) {
        std::cerr << "vstudio-cli: --log-level needs a value\n";
        return 2;
      }
      const auto level = vs::core::parse_log_level(args[++i]);
      if (!level) {
        std::cerr << "vstudio-cli: unknown log level '" << args[i] << "'\n";
        return 2;
      }
      vs::core::set_log_level(*level);
    } else if (arg.starts_with("-")) {
      std::cerr << "vstudio-cli: unknown option '" << arg << "'\n" << kUsage;
      return 2;
    } else {
      command = arg;
    }
  }

  if (command.empty() || command == "help") {
    std::cout << kUsage;
    return command.empty() ? 2 : 0;
  }
  if (command == "version") {
    print_version(std::cout);
    return 0;
  }
  if (command == "selftest") {
    print_version(std::cout);
    return vs::cli::run_selftest(std::cout) ? 0 : 1;
  }
  if (command == "kernel-demo") {
    return vs::cli::run_kernel_demo(rest, std::cout, std::cerr);
  }
  if (command == "sketch-solve") {
    return vs::cli::run_sketch_solve(rest, std::cout, std::cerr);
  }
  if (command == "tolerance-spike") {
    return vs::cli::run_tolerance_spike(rest, std::cout, std::cerr);
  }

  std::cerr << "vstudio-cli: unknown command '" << command << "'\n" << kUsage;
  return 2;
}

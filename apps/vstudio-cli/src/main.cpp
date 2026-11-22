// vstudio-cli: command-line driver for Volumetric CAD Studio.

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "bench_commands.hpp"
#include "config_commands.hpp"
#include "eval_commands.hpp"
#include "proc_commands.hpp"
#include "feature_commands.hpp"
#include "kernel_commands.hpp"
#include "model_commands.hpp"
#include "sketch_commands.hpp"
#include "mesh_commands.hpp"
#include "validate_commands.hpp"
#include "selftest.hpp"
#include "vs/core/log.hpp"
#include "vs/core/version.hpp"
#include "vs/gpu/gpu.hpp"

namespace {

constexpr std::string_view kUsage = R"(usage: vstudio-cli [options] <command> [args]

Commands:
  selftest          Run built-in smoke checks of the linked libraries
  kernel-demo [f]   Build the sample part and write it as STEP (default: kernel-demo.step)
  tolerance-spike   Run the M1 kernel tolerance spike and print its measurements
  sketch-solve <in.json> [out.json]
                    Solve a sketch file, report it and its profiles, optionally write it
  build <in.json> [out.step]
                    Rebuild a feature tree file, report it, optionally write the body as STEP
  info <project.vsproj> [--build]
                    Print a project's volumes, connection surfaces and dependencies;
                    with --build, also build it and print statuses and outputs
  validate <project.vsproj>
                    Validate a project (overlap, contact, connection rules, loops,
                    containment); print each problem and where to fix it.
                    Exits 1 when there are errors
  seams <project.vsproj>
                    Build a project and print each connection surface's seam
                    (spacing, vertices, triangles); exits 1 if one fails
  export <project.vsproj> --stl <file> | --3mf <file> [--per-volume] [--placeholders]
         [--deflection <mm>]
                    Tessellate every volume against its seams, weld them and write
                    the combined mesh (3MF: one object, or one per volume). Writes
                    nothing and exits 1 unless it is watertight and manifold
  import <project.vsproj> <file.step|.iges|.stl|.3mf|.obj> [--name <name> | --into <volume>]
         [--out <project>]
                    Add a file as a static volume boxed by its bounds (or, with --into,
                    as an existing volume's contents), and save
  rebuild <project.vsproj> [--threads N] [--check-determinism] [--weld] [--stl <out.stl>]
          [--3mf <out.3mf> [--per-volume]] [--step <out.step> [--procedural-boundary]]
          [--cache] [--set <volume>.<parameter>=<value>]... [--twice]
                    Evaluate a project with the engine: regenerate what changed (meshing
                    procedural volumes), check containment, publish outputs; print
                    statuses, cache use and the output hash. --stl and --3mf write the
                    welded mesh when it is clean (3MF: one object, or one per volume);
                    --step writes STEP AP242, one named part per volume (procedural
                    ones as mesh bodies, or their boundary). --cache uses the project's
                    cache/ (a zip project's is read, then saved back into the zip).
                    Exits 1 on errors
  configs <project.vsproj> [--build] [--threads N] [--activate <name>|-]
                    List a project's configurations and their values; --build evaluates
                    each (sharing one cache) and prints statuses and published outputs;
                    --activate makes one the active configuration ("-": the authored
                    values) and saves the project. Exits 1 on errors
  design-table <project.vsproj> [--all] [--out <table.csv>]
  design-table <project.vsproj> --import <table.csv> [--out <project>]
                    Write the configurations as a CSV design table (--all: a column for
                    every variable and parameter that can be driven), or make a table
                    the project's configurations and save it
  nodes             List the procedural node library: types, ports and properties
  graph <graph.json>
                    Check a procedural graph (types, loops, settings) and print its
                    nodes in evaluation order. Exits 1 on errors
  templates [<name> [--surface <id>] [--cell <mm>]]
                    List the starter procedural templates, or print one as graph JSON
  inspect <project.vsproj> <volume> <node> [--preview]
                    Evaluate one node of a procedural volume's graph (and what it
                    depends on) and print its outputs; --preview meshes fields
  bake <project.vsproj> <volume> [--out <project>]
                    Generate a procedural volume and bake it into a static 3MF volume
                    that keeps its graph, and save
  unbake <project.vsproj> <volume> [--out <project>]
                    Restore a baked volume's graph, and save
  bench-gyroid [--size <mm>] [--cell <mm>] [--threads N] [--3mf <file>]
                    Time the gyroid_infill template in a cube (default 100 mm at a
                    0.5 mm grid), stage by stage: the CPU baseline. --threads takes
                    0 (every core, the default) to 256
  bench-edit [--volumes N] [--repeat R] [--no-reuse] [--project <project.vsproj>]
                    Time what an edit costs besides generating: the model check,
                    the preview tier and the full evaluation, on rows of N
                    parametric boxes (default 20) with two links, or how long a
                    project's check, layout and re-evaluation take. --no-reuse
                    builds and checks everything every time, as before 0029
  bench-rack [--cells N[,N...]] [--cell <mm>] [--threads N] [--csv <file>]
                    Time Engine::preview() on the rack fixture (a composite whose
                    generator makes a grid of cells) at 10, 100, 1,000 and 10,000
                    cells, phase by phase with peak memory (Linux): how the preview
                    tier scales
  version           Print the version (same as --version)
  help              Print this message

Options:
  --version         Print the version and exit
  --gpu             Sample and mesh procedural fields on the GPU where it gives
                    exactly the CPU's results and is faster (it says which); for
                    rebuild, inspect, bake and bench-gyroid, ignored by the rest
  --gpu=force       The same, faster or not (benchmarks)
  --log-level <l>   trace, debug, info, warning, error or off (default: warning)
  -h, --help        Print this message and exit
)";

// Sampling (L1) and meshing (L1b) on the GPU where it matches the CPU.
void install_gpu(bool force) {
  vs::gpu::InstallOptions options;
  options.force = force;
  const auto status = vs::gpu::install(options);
  if (status.installed) {
    const auto where = [](bool gpu) { return gpu ? "GPU" : "CPU"; };
    std::cout << "gpu         " << status.device << ": sampling on the " << where(status.sampling)
              << ", clip on the " << where(status.clip) << ", unmerged net on the "
              << where(status.meshing) << " (checks and calibration "
              << static_cast<long>(status.install_ms) << " ms)\n";
    if (!status.calibration.empty()) std::cout << "            " << status.calibration << "\n";
    if (!status.why.empty()) std::cout << "            " << status.why << "\n";
  } else {
    std::cout << "gpu         not used: " << status.why << "\n";
  }
}

// The commands that sample procedural fields, and so may use the GPU.
bool samples_fields(std::string_view command) {
  return command == "rebuild" || command == "inspect" || command == "bake" ||
         command == "bench-gyroid";
}

void print_version(std::ostream& out) {
  out << "vstudio-cli " << vs::core::version_string() << " (" << vs::core::git_revision()
      << ")\n";
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);

  vs::core::set_log_level(vs::core::LogLevel::Warning);

  std::string_view command;
  std::string_view gpu;  // --gpu or --gpu=force, installed only for a command that samples
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
    } else if (arg == "--gpu" || arg == "--gpu=force") {
      gpu = arg;
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
  // Opening a GL context and calibrating takes seconds; only the commands
  // that sample fields pay for it (H4).
  if (!gpu.empty() && samples_fields(command)) install_gpu(gpu == "--gpu=force");
  // Whatever opened the GPU (--gpu, selftest) closes it before main
  // returns, never in static destructors (H6).
  struct GpuShutdown {
    ~GpuShutdown() { vs::gpu::shutdown(); }
  } gpu_shutdown;
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
  if (command == "build") {
    return vs::cli::run_build(rest, std::cout, std::cerr);
  }
  if (command == "info") {
    return vs::cli::run_info(rest, std::cout, std::cerr);
  }
  if (command == "validate") {
    return vs::cli::run_validate(rest, std::cout, std::cerr);
  }
  if (command == "seams") {
    return vs::cli::run_seams(rest, std::cout, std::cerr);
  }
  if (command == "export") {
    return vs::cli::run_export(rest, std::cout, std::cerr);
  }
  if (command == "import") {
    return vs::cli::run_import(rest, std::cout, std::cerr);
  }
  if (command == "configs") {
    return vs::cli::run_configs(rest, std::cout, std::cerr);
  }
  if (command == "design-table") {
    return vs::cli::run_design_table(rest, std::cout, std::cerr);
  }
  if (command == "rebuild") {
    return vs::cli::run_rebuild(rest, std::cout, std::cerr);
  }
  if (command == "nodes") {
    return vs::cli::run_nodes(rest, std::cout, std::cerr);
  }
  if (command == "graph") {
    return vs::cli::run_graph(rest, std::cout, std::cerr);
  }
  if (command == "templates") {
    return vs::cli::run_templates(rest, std::cout, std::cerr);
  }
  if (command == "bench-edit") {
    return vs::cli::run_bench_edit(rest, std::cout, std::cerr);
  }
  if (command == "bench-rack") {
    return vs::cli::run_bench_rack(rest, std::cout, std::cerr);
  }
  if (command == "bench-gyroid") {
    return vs::cli::run_bench_gyroid(rest, std::cout, std::cerr);
  }
  if (command == "inspect") {
    return vs::cli::run_inspect(rest, std::cout, std::cerr);
  }
  if (command == "bake") {
    return vs::cli::run_bake(rest, std::cout, std::cerr);
  }
  if (command == "unbake") {
    return vs::cli::run_unbake(rest, std::cout, std::cerr);
  }
  if (command == "tolerance-spike") {
    return vs::cli::run_tolerance_spike(rest, std::cout, std::cerr);
  }

  std::cerr << "vstudio-cli: unknown command '" << command << "'\n" << kUsage;
  return 2;
}

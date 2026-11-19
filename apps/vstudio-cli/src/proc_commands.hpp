#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vs::cli {

// `nodes`: lists the node library: each type with its inputs, outputs and
// properties. Returns 0.
int run_nodes(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

// `graph <graph.json>`: reads a procedural graph, checks it (types, loops,
// settings, one output) and prints its nodes in evaluation order with what
// feeds each input. Returns 0 when it checks clean, 1 on errors (printed),
// 2 for bad arguments.
int run_graph(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

// `templates [<name> [--surface <id>] [--cell <mm>]]`: lists the starter
// templates, or prints one as graph JSON. Returns 0, 1 for an unknown
// template, 2 for bad arguments.
int run_templates(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

// `bench-gyroid [--size <mm>] [--cell <mm>] [--threads N] [--3mf <file>]`: generates the
// gyroid_infill template in a cube `size` mm on a side (100) at `cell` mm (0.5) through the
// engine, and times each stage (inputs, sampling, meshing, containment). The CPU baseline
// for the GPU milestone (plan, M12). Returns 0, 1 if generation fails.
int run_bench_gyroid(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

// `inspect <project> <volume> <node> [--preview]`: evaluates one node of a procedural
// volume's graph (and what it depends on) and prints its outputs; --preview also meshes
// field outputs as the node editor would. Returns 0, 1 on errors.
int run_inspect(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

// `bake <project> <volume> [--out <project>]`: generates the volume and bakes it into a
// static 3MF volume that keeps its graph; `unbake <project> <volume> [--out <project>]`
// restores the graph. Returns 0, 1 on errors.
int run_bake(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);
int run_unbake(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vs::cli

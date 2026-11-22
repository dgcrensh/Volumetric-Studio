#include "bench_commands.hpp"
#include "files.hpp"
#include "memory.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "vs/core/settings.hpp"
#include "vs/eval/engine.hpp"
#include "vs/feature/tree.hpp"
#include "vs/model/document.hpp"
#include "vs/model/project.hpp"
#include "vs/proc/generator.hpp"
#include "vs/proc/graph.hpp"

namespace vs::cli {

namespace {

using core::dvec3;
using core::Id;
using feature::FaceRef;
using feature::FeatureRef;
using feature::Param;

double ms_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// A rectangle covering the "+z" face of the box `boundary` names (a volume
// or a template), extruded down by `depth`: the rack fixture's fill
// (libs/vs_model/tests/rack.cpp, cell_tree()).
model::ParametricContents fill_tree(const Id& boundary, double x, double y, Param depth) {
  model::ParametricContents contents;
  feature::FeatureTree& t = contents.tree;
  sketch::Sketch s(Id::random(), "Outline");
  const Id a = s.add_point({0, 0});
  const Id b = s.add_point({x, 0});
  const Id c = s.add_point({x, y});
  const Id d = s.add_point({0, y});
  s.add_line(a, b);
  s.add_line(b, c);
  s.add_line(c, d);
  s.add_line(d, a);
  const Id sketch = t.add(feature::SketchFeature{s, FaceRef{boundary, "+z"}}, "Outline");
  feature::ExtrudeFeature e;
  e.sketch = sketch;
  e.depth = std::move(depth);
  e.reverse = true;  // down, into the box
  t.add(e, "Fill");
  return contents;
}

// The rack fixture (libs/vs_model/tests/rack.hpp) at any size: a Base under
// a Rack composite whose generator (children.grid, bonded neighbours, the
// Seat delegated to the cell at the corner) splits it into nx * ny cells of
// `cell` x `cell` x 10 mm, centred on the origin. Cells take the fixture's
// Cell template (a parametric fill, parameter `depth`, a volume output).
model::Project make_rack(std::size_t nx, std::size_t ny, double cell) {
  model::Project project;
  model::Document& doc = project.document;
  doc = model::make_document("Rack bench");
  const double w = static_cast<double>(nx) * cell;
  const double h = static_cast<double>(ny) * cell;
  const dvec3 lo{-w / 2, -h / 2, 0};
  const auto point = [&](double x, double y, double z, std::string name) {
    return doc.layout.add(
        feature::DatumPointFeature{feature::PointAt{Param::of(x), Param::of(y), Param::of(z)}},
        std::move(name));
  };
  const Id base_lo = point(lo.x, lo.y, -10, "Base low");
  const Id origin = point(lo.x, lo.y, 0, "Rack low");
  const Id base_hi = point(-lo.x, -lo.y, 0, "Base high");
  const Id rack_hi = point(-lo.x, -lo.y, 10, "Rack high");

  model::Volume base;
  base.id = Id::random();
  base.name = "Base";
  base.boundary = model::BoxBoundary{FeatureRef{base_lo}, FeatureRef{base_hi}};
  base.contents = fill_tree(base.id, w, h, Param::of(10));

  model::Volume rack;
  rack.id = Id::random();
  rack.name = "Rack";
  rack.boundary = model::BoxBoundary{FeatureRef{origin}, FeatureRef{rack_hi}};
  model::CompositeContents contents;
  model::ChildTemplate tmpl;
  tmpl.id = Id::random();
  tmpl.name = "Cell";
  tmpl.contents = fill_tree(tmpl.id, cell, cell, Param::expr("depth"));
  tmpl.parameters.push_back({Id::random(), "depth", Param::of(10)});
  tmpl.outputs.push_back({Id::random(), "volume", model::OutputQuantity::Volume, 1.0});
  rack.outputs.push_back({Id::random(), "volume", model::OutputQuantity::Volume, 1.0});

  model::DatumPatch seat;
  seat.plane = feature::PrincipalPlane::XY;
  seat.u = Param::of(lo.x + cell / 2);
  seat.v = Param::of(lo.y + cell / 2);
  seat.width = Param::of(cell * 0.8);
  seat.height = Param::of(cell * 0.8);
  model::ConnectionSurface s;
  s.id = Id::random();
  s.name = "Seat";
  s.geometry = seat;
  s.params = model::BondedParams{};
  s.volumes = {base.id, rack.id};

  proc::Graph g;
  const Id grid = g.add("children.grid", "Grid",
                        {{"count", proc::Setting(dvec3{static_cast<double>(nx),
                                                       static_cast<double>(ny), 1.0})},
                         {"template", proc::Setting(std::string("Cell"))},
                         {"join", proc::Setting(std::string("bonded"))}});
  const Id delegate = g.add("children.delegate", "Seat",
                            {{"surface", proc::Setting(s.id.to_string())}});
  const Id output = g.add("output.children", "Children");
  g.link(grid, "children", delegate, "children");
  g.link(delegate, "children", output, "children");
  contents.generator = proc::to_json(g);
  contents.templates.push_back(std::move(tmpl));
  rack.contents = std::move(contents);

  doc.volumes = {std::move(base), std::move(rack)};
  doc.connections = {std::move(s)};
  return project;
}

// nx * ny = cells with nx >= ny as close as they come.
std::pair<std::size_t, std::size_t> near_square(std::size_t cells) {
  std::size_t ny = static_cast<std::size_t>(std::sqrt(static_cast<double>(cells)));
  while (ny > 1 && cells % ny != 0) --ny;
  return {cells / ny, ny};
}

// One phase's totals over an evaluation (a phase may run once per wave or
// per generating composite).
struct PhaseTotal {
  std::string name;
  std::size_t runs = 0;
  double ms = 0.0;
  std::optional<double> peak_mb;   // the process's peak RSS during it
  std::optional<double> grown_mb;  // that peak over the RSS when it started
};

struct Profile {
  std::vector<PhaseTotal> phases;  // in the order they first ran
  double total_ms = 0.0;
  std::optional<double> peak_mb;
  eval::Evaluation evaluation;
};

std::optional<double> max_of(std::optional<double> a, std::optional<double> b) {
  if (!a) return b;
  if (!b) return a;
  return std::max(*a, *b);
}

// engine.preview(project), timed phase by phase through PhaseFinished
// events (vs/eval/cancel.hpp). The phases run on this thread one after
// another, so each one's time and peak memory are the span since the last.
Profile profile_preview(eval::Engine& engine, const model::Project& project) {
  Profile p;
  std::map<std::string, std::size_t, std::less<>> index;
  eval::EvaluateOptions options;
  options.generate = false;  // preview(), with progress
  auto mark = std::chrono::steady_clock::now();
  std::optional<double> start_mb;
  options.progress = [&](const eval::ProgressEvent& e) {
    if (e.kind != eval::ProgressEvent::Kind::PhaseFinished || e.phase == nullptr) return;
    const double ms = ms_since(mark);
    const auto peak = peak_rss_mb();
    auto [it, added] = index.try_emplace(e.phase, p.phases.size());
    if (added) {
      PhaseTotal fresh;
      fresh.name = e.phase;
      p.phases.push_back(std::move(fresh));
    }
    PhaseTotal& t = p.phases[it->second];
    ++t.runs;
    t.ms += ms;
    t.peak_mb = max_of(t.peak_mb, peak);
    if (peak && start_mb) t.grown_mb = max_of(t.grown_mb, std::max(0.0, *peak - *start_mb));
    p.peak_mb = max_of(p.peak_mb, peak);
    reset_peak_rss();
    start_mb = current_rss_mb();
    mark = std::chrono::steady_clock::now();
  };
  reset_peak_rss();
  start_mb = current_rss_mb();
  const auto t0 = std::chrono::steady_clock::now();
  mark = t0;
  p.evaluation = engine.evaluate(project, options);
  p.total_ms = ms_since(t0);
  p.peak_mb = max_of(p.peak_mb, peak_rss_mb());
  return p;
}

std::string mb_text(std::optional<double> mb) {
  return mb ? fmt::format("{:8.0f}", *mb) : std::string("       -");
}

std::optional<std::size_t> whole(std::string_view text, std::size_t lo, std::size_t hi) {
  std::size_t n = 0;
  const auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
  if (ec != std::errc() || p != text.data() + text.size() || n < lo || n > hi) return std::nullopt;
  return n;
}

}  // namespace

int run_bench_rack(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  constexpr std::string_view kUsage =
      "usage: vstudio-cli bench-rack [--cells N[,N...]] [--cell <mm>] [--threads N] "
      "[--csv <file>]\n";
  constexpr std::size_t kMaxCells = 10000;  // children.grid's limit
  std::vector<std::size_t> scales{10, 100, 1000, 10000};
  double cell = 8.0;
  std::size_t threads = 0;
  std::optional<std::string_view> csv_path;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const bool has_value = i + 1 < args.size();
    if (args[i] == "--cells" && has_value) {
      scales.clear();
      std::string_view list = args[++i];
      while (!list.empty()) {
        const auto comma = list.find(',');
        const auto n = whole(list.substr(0, comma), 1, kMaxCells);
        if (!n) return err << "vstudio-cli: --cells takes numbers from 1 to 10000, comma separated\n", 2;
        scales.push_back(*n);
        list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
      }
      if (scales.empty()) return err << kUsage, 2;
    } else if (args[i] == "--cell" && has_value) {
      const std::string_view t = args[++i];
      double v = 0;
      const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
      // A cell needs room for a joining patch inside each face (margin 1 mm).
      if (ec != std::errc() || p != t.data() + t.size() || !(v >= 4.0) || !(v <= 100.0)) {
        return err << "vstudio-cli: --cell takes 4 to 100 (mm)\n", 2;
      }
      cell = v;
    } else if (args[i] == "--threads" && has_value) {
      const auto n = whole(args[++i], 0, 256);
      if (!n) return err << "vstudio-cli: --threads takes a number from 0 (every core) to 256\n", 2;
      threads = *n;
    } else if (args[i] == "--csv" && has_value) {
      csv_path = args[++i];
    } else {
      return err << kUsage, 2;
    }
  }
  const auto settings = core::ProductSettings::defaults();
  for (const std::size_t n : scales) {
    const auto [nx, ny] = near_square(n);
    const double half = static_cast<double>(std::max(nx, ny)) * cell / 2;
    if (half > settings.working_half_extent_mm) {
      err << fmt::format("vstudio-cli: {} cells of {} mm ({} x {}) leave the working space; use a "
                         "smaller --cell\n",
                         n, cell, nx, ny);
      return 2;
    }
  }

  std::string csv = "cells,run,phase,runs,ms,peak_mb,grown_mb\n";
  const auto csv_mb = [](std::optional<double> mb) {
    return mb ? fmt::format("{:.1f}", *mb) : std::string();
  };
  struct Row {
    std::size_t cells;
    std::size_t volumes;
    double cold_ms, warm_ms;
    std::optional<double> peak_mb;
  };
  std::vector<Row> summary;
  bool failed = false;
  bool first = true;
  for (const std::size_t n : scales) {
    const auto [nx, ny] = near_square(n);
    auto t = std::chrono::steady_clock::now();
    const model::Project project = make_rack(nx, ny, cell);
    const double scene_ms = ms_since(t);
    eval::EngineOptions eo;
    eo.threads = threads;
    eval::Engine engine(settings, eo);
    proc::install(engine);
    if (first) {
      out << fmt::format("bench-rack  preview of the rack fixture: a Rack of {} mm cells "
                         "(children.grid, bonded), {} threads\n",
                         cell, engine.pool().size());
      first = false;
    }

    Profile cold = profile_preview(engine, project);
    const eval::Evaluation& ev = cold.evaluation;
    std::size_t generated = 0;
    for (const auto& v : ev.document.volumes) generated += v.generated ? 1 : 0;
    out << fmt::format("\ncells {} ({} x {}): {} volumes, {} connection surfaces after expansion; "
                       "scene {:.1f} ms\n",
                       n, nx, ny, ev.document.volumes.size(), ev.document.connections.size(),
                       scene_ms);
    out << "  phase                        runs        ms    % peak MB   +MB\n";
    double accounted = 0.0;
    for (const auto& ph : cold.phases) {
      accounted += ph.ms;
      out << fmt::format("  {:<26} {:6} {:9.1f} {:4.0f}% {} {:>5}\n", ph.name, ph.runs, ph.ms,
                         100.0 * ph.ms / std::max(cold.total_ms, 1e-9), mb_text(ph.peak_mb),
                         ph.grown_mb ? fmt::format("{:.0f}", *ph.grown_mb) : std::string("-"));
      csv += fmt::format("{},cold,{},{},{:.3f},{},{}\n", n, ph.name, ph.runs, ph.ms,
                         csv_mb(ph.peak_mb), csv_mb(ph.grown_mb));
    }
    out << fmt::format("  {:<26} {:6} {:9.1f}\n", "(between phases)", "",
                       std::max(0.0, cold.total_ms - accounted));
    out << fmt::format("  {:<26} {:6} {:9.1f}       {}\n", "preview", "", cold.total_ms,
                       mb_text(cold.peak_mb));
    csv += fmt::format("{},cold,total,1,{:.3f},{},\n", n, cold.total_ms, csv_mb(cold.peak_mb));

    if (!ev.checked || generated != n) {
      out << fmt::format("  expected {} generated cells, got {}\n", n, generated);
      failed = true;
    }
    std::size_t shown = 0;
    for (const auto& d : ev.diagnostics) {
      if (!d.is_error()) continue;
      failed = true;
      if (shown++ < 5) out << "  " << d.to_string() << "\n";
    }

    // The same preview again: the generated layout comes from the engine's
    // cache, everything else runs as before (what an edit elsewhere costs).
    Profile warm = profile_preview(engine, project);
    out << fmt::format("  {:<26} {:6} {:9.1f}       {}  (layouts generated {})\n",
                       "preview again", "", warm.total_ms, mb_text(warm.peak_mb),
                       warm.evaluation.stats.layouts_generated);
    for (const auto& ph : warm.phases) {
      csv += fmt::format("{},warm,{},{},{:.3f},{},{}\n", n, ph.name, ph.runs, ph.ms,
                         csv_mb(ph.peak_mb), csv_mb(ph.grown_mb));
    }
    csv += fmt::format("{},warm,total,1,{:.3f},{},\n", n, warm.total_ms, csv_mb(warm.peak_mb));
    summary.push_back({n, ev.document.volumes.size(), cold.total_ms, warm.total_ms, cold.peak_mb});
    out.flush();  // each size as it finishes: the large ones take a while
  }

  out << "\nsummary      cells   volumes  preview ms  ms per cell  again ms  peak MB\n";
  for (const Row& r : summary) {
    out << fmt::format("            {:6} {:9} {:11.1f} {:12.3f} {:9.1f} {}\n", r.cells, r.volumes,
                       r.cold_ms, r.cold_ms / static_cast<double>(r.cells), r.warm_ms,
                       mb_text(r.peak_mb));
  }
  // How each step up in size scales: time grows as cells^k.
  for (std::size_t i = 1; i < summary.size(); ++i) {
    const Row& a = summary[i - 1];
    const Row& b = summary[i];
    if (b.cells == a.cells || a.cold_ms <= 0.0) continue;
    const double k = std::log(b.cold_ms / a.cold_ms) /
                     std::log(static_cast<double>(b.cells) / static_cast<double>(a.cells));
    out << fmt::format("growth      {} -> {} cells: time ~ cells^{:.2f}\n", a.cells, b.cells, k);
  }
  if (!peak_rss_mb()) out << "memory      not measured here (Linux only)\n";
  if (csv_path) {
    const auto* bytes = reinterpret_cast<const std::byte*>(csv.data());
    if (!write_bytes(to_path(*csv_path), std::span(bytes, csv.size()), err)) return 1;
    out << "wrote       " << *csv_path << "\n";
  }
  out << (failed ? "result      errors\n" : "result      ok\n");
  return failed ? 1 : 0;
}

}  // namespace vs::cli

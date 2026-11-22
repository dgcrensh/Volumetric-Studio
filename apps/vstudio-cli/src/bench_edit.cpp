// bench-edit: what an interactive edit costs outside generation (decision
// 0029). Builds rows of parametric boxes with two links in code, then
// times the app's path for three kinds of edit: History's check, the
// session's preview tier and its full evaluation.

#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "eval_commands.hpp"
#include "vs/core/settings.hpp"
#include "vs/eval/engine.hpp"
#include "vs/feature/rebuild.hpp"
#include "vs/feature/tree.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"
#include "vs/model/project.hpp"
#include "vs/validate/layout.hpp"

namespace vs::cli {

namespace {

using core::dvec2;
using core::dvec3;
using core::Id;
using feature::DatumPointFeature;
using feature::ExtrudeFeature;
using feature::FaceRef;
using feature::FeatureRef;
using feature::Param;
using feature::PointAt;
using feature::SketchFeature;
using sketch::Sketch;

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void rectangle(Sketch& sketch, dvec2 lo, dvec2 hi) {
  Id p[4];
  Id l[4];
  p[0] = sketch.add_point(lo);
  p[1] = sketch.add_point({hi.x, lo.y});
  p[2] = sketch.add_point(hi);
  p[3] = sketch.add_point({lo.x, hi.y});
  for (int i = 0; i < 4; ++i) l[i] = sketch.add_line(p[i], p[(i + 1) % 4]);
  sketch.fix(p[0]);
  sketch.horizontal(l[0]);
  sketch.horizontal(l[2]);
  sketch.vertical(l[1]);
  sketch.vertical(l[3]);
  sketch.length(l[0], hi.x - lo.x);
  sketch.length(l[1], hi.y - lo.y);
}

struct Scene {
  model::Project project;
  std::vector<Id> volumes;
  std::vector<Id> depth;
  std::vector<Id> fill;  // each volume's extrude
};

// n boxes 20 mm wide, 5 mm apart in rows of 20 along x, each filled by a 16 x 16 pocket
// extruded `depth` (10 mm) down from its top. V1's depth is fed by V0's
// volume through a link (a volume parameter target), and the layout
// variable `w` too (a layout target): the last box is `w / 128` tall.
Scene make_scene(std::size_t n) {
  Scene s;
  model::Document& doc = s.project.document;
  doc = model::make_document("Bench");
  (void)doc.layout.variables().add("w", "2560");
  for (std::size_t i = 0; i < n; ++i) {
    const double x0 = 25.0 * static_cast<double>(i % 20);
    const double y0 = 25.0 * static_cast<double>(i / 20);
    const std::string name = fmt::format("V{}", i);
    model::Volume v;
    v.id = Id::random();
    v.name = name;
    const Param top = i + 1 == n ? Param::expr("w / 128") : Param::of(20);
    const Id lo = doc.layout.add(
        DatumPointFeature{PointAt{Param::of(x0), Param::of(y0), Param::of(0)}}, name + " min");
    const Id hi = doc.layout.add(
        DatumPointFeature{PointAt{Param::of(x0 + 20), Param::of(y0 + 20), top}}, name + " max");
    v.boundary = model::BoxBoundary{FeatureRef{lo}, FeatureRef{hi}};
    const Id depth = Id::random();
    v.parameters.push_back({depth, i == 1 ? "feed" : "depth", Param::of(i == 1 ? 2560 : 10)});
    v.outputs.push_back({Id::random(), "volume", model::OutputQuantity::Volume, 1.0});
    v.seed = i;
    model::ParametricContents contents;
    Sketch sketch(Id::random(), "Outline");
    rectangle(sketch, {x0 + 2, y0 + 2}, {x0 + 18, y0 + 18});
    const Id sk = contents.tree.add(SketchFeature{sketch, FaceRef{v.id, "+z"}});
    ExtrudeFeature fill;
    fill.sketch = sk;
    fill.depth = Param::expr(i == 1 ? "feed / 256" : "depth");
    fill.reverse = true;
    s.fill.push_back(contents.tree.add(fill, "Fill"));
    v.contents = std::move(contents);
    s.volumes.push_back(v.id);
    s.depth.push_back(depth);
    doc.volumes.push_back(std::move(v));
  }
  const model::Volume& v0 = doc.volumes[0];
  doc.links.push_back({Id::random(), {v0.id, v0.outputs[0].id}, s.depth[1]});
  doc.links.push_back({Id::random(), {v0.id, v0.outputs[0].id}, doc.layout.variables().find("w")->id});
  return s;
}

void set_param(model::Document& doc, const Id& volume, const Id& parameter, double value) {
  for (auto& p : doc.find_volume(volume)->parameters) {
    if (p.id == parameter) p.value = Param::of(value);
  }
}

struct Timing {
  double apply = 0, preview = 0, full = 0;
  std::size_t preview_layouts = 0, full_layouts = 0;
};

}  // namespace

int run_bench_edit(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  std::size_t n = 20;
  std::size_t repeat = 10;
  std::string project_path;
  bool reuse = true;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto next = [&]() -> long {
      if (i + 1 >= args.size()) return -1;
      return std::strtol(std::string(args[++i]).c_str(), nullptr, 10);
    };
    if (args[i] == "--volumes") {
      const long v = next();
      if (v < 3 || v > 10000) {
        err << "bench-edit: --volumes takes 3 to 10000\n";
        return 2;
      }
      n = static_cast<std::size_t>(v);
    } else if (args[i] == "--repeat") {
      const long v = next();
      if (v < 1 || v > 10000) {
        err << "bench-edit: --repeat takes 1 to 10000\n";
        return 2;
      }
      repeat = static_cast<std::size_t>(v);
    } else if (args[i] == "--no-reuse") {
      reuse = false;
    } else if (args[i] == "--project" && i + 1 < args.size()) {
      project_path = std::string(args[++i]);
    } else {
      err << "bench-edit: unknown argument '" << args[i] << "'\n";
      return 2;
    }
  }

  const core::ProductSettings settings;
  if (!project_path.empty()) {
    auto loaded = model::load_project(project_path);
    if (!loaded) {
      err << "bench-edit: " << loaded.error().message << "\n";
      return 1;
    }
    const model::Project project = std::move(loaded).value();
    const model::Document& doc = project.document;
    const int k = 20;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < k; ++i) (void)model::check(doc);
    const double check_ms = ms_since(t0) / k;
    t0 = std::chrono::steady_clock::now();
    model::DocumentBuild b;
    for (int i = 0; i < 5; ++i) b = model::build_layout(doc, settings);
    const double layout_ms = ms_since(t0) / 5;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) (void)feature::rebuild(doc.layout, settings);
    const double tree_ms = ms_since(t0) / 5;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) (void)validate::validate_layout(doc, b, settings);
    const double validate_ms = ms_since(t0) / 5;
    out << fmt::format("project     {} ({} volumes)\n", project_path, doc.volumes.size());
    out << fmt::format("check       {:.2f} ms\n", check_ms);
    out << fmt::format("layout      {:.2f} ms build ({:.2f} ms its tree), {:.2f} ms validate\n",
                       layout_ms, tree_ms, validate_ms);
    eval::EngineOptions peo;
    peo.threads = 4;
    peo.reuse_layouts = reuse;
    eval::Engine engine(settings, peo);
    (void)engine.evaluate(project);
    double pv = 0, full = 0;
    std::size_t builds = 0;
    for (std::size_t r = 0; r < repeat; ++r) {
      t0 = std::chrono::steady_clock::now();
      builds += engine.preview(project).stats.layout_builds;
      pv += ms_since(t0);
      t0 = std::chrono::steady_clock::now();
      builds += engine.evaluate(project).stats.layout_builds;
      full += ms_since(t0);
    }
    const double rr = static_cast<double>(repeat);
    out << fmt::format("again       {:.2f} ms preview, {:.2f} ms full, {:.1f} layouts built\n",
                       pv / rr, full / rr, static_cast<double>(builds) / rr);
    return 0;
  }
  Scene scene = make_scene(n);
  if (const auto d = model::check(scene.project.document); d.has_errors()) {
    err << "bench-edit: the scene doesn't check: " << d[0].code << " " << d[0].message << "\n";
    return 1;
  }

  // The parts on their own.
  {
    const model::Document& doc = scene.project.document;
    const int k = 20;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < k; ++i) (void)model::check(doc);
    const double check_ms = ms_since(t0) / k;
    t0 = std::chrono::steady_clock::now();
    model::DocumentBuild b;
    for (int i = 0; i < 3; ++i) b = model::build_layout(doc, settings);
    const double layout_ms = ms_since(t0) / 3;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) (void)validate::validate_layout(doc, b, settings);
    const double validate_ms = ms_since(t0) / 3;
    out << fmt::format("scene       {} volumes, 2 links (a parameter, a layout variable); {}\n", n,
                       reuse ? "layouts reused" : "--no-reuse");
    out << fmt::format("check       {:.2f} ms\n", check_ms);
    out << fmt::format("layout      {:.2f} ms build, {:.2f} ms validate\n", layout_ms, validate_ms);
  }

  eval::EngineOptions eo;
  eo.threads = 4;
  eo.reuse_layouts = reuse;
  eval::Engine engine(settings, eo);
  {
    const auto ev = engine.evaluate(scene.project);
    if (ev.diagnostics.has_errors()) {
      err << "bench-edit: the scene doesn't evaluate cleanly\n";
      return 1;
    }
  }

  struct Kind {
    const char* name;
    std::function<void(model::Document&, std::size_t)> change;
  };
  const std::vector<Kind> kinds{
      {"contents", [&](model::Document& doc, std::size_t k) {
         auto* tree = doc.find_volume(scene.volumes[2])->tree();
         auto* f = tree->find(scene.fill[2]);
         std::get<ExtrudeFeature>(f->definition).depth =
             Param::expr(fmt::format("depth - {}", 0.01 * static_cast<double>(k + 1)));
       }},
      {"parameter", [&](model::Document& doc, std::size_t k) {
         set_param(doc, scene.volumes[3], scene.depth[3], 10 - 0.01 * static_cast<double>(k + 1));
       }},
      {"upstream", [&](model::Document& doc, std::size_t k) {
         set_param(doc, scene.volumes[0], scene.depth[0], 10 - 0.01 * static_cast<double>(k + 1));
       }},
  };

  out << fmt::format("{:<10} {:>10} {:>10} {:>8} {:>10} {:>8}   (ms per edit, mean of {})\n", "edit",
                     "apply", "preview", "layouts", "full", "layouts", repeat);
  bool ok = true;
  // History keeps the current state's findings (decision 0029); --no-reuse
  // checks it again on every edit, as before.
  core::Diagnostics current = model::check(scene.project.document);
  for (const Kind& kind : kinds) {
    Timing t;
    for (std::size_t k = 0; k < repeat; ++k) {
      // History::apply: the change on a copy, checked against the current state.
      auto t0 = std::chrono::steady_clock::now();
      model::Project next = scene.project;
      kind.change(next.document, k);
      if (!reuse) current = model::check(scene.project.document);
      const auto after = model::check(next.document);
      t.apply += ms_since(t0);
      if (after.has_errors()) ok = false;
      current = after;
      scene.project = std::move(next);
      // Session: the preview tier, then the full evaluation.
      t0 = std::chrono::steady_clock::now();
      const auto pv = engine.preview(scene.project);
      t.preview += ms_since(t0);
      t.preview_layouts += pv.stats.layout_builds;
      t0 = std::chrono::steady_clock::now();
      const auto ev = engine.evaluate(scene.project);
      t.full += ms_since(t0);
      t.full_layouts += ev.stats.layout_builds;
      if (ev.diagnostics.has_errors()) ok = false;
    }
    const double r = static_cast<double>(repeat);
    out << fmt::format("{:<10} {:>10.2f} {:>10.2f} {:>8.1f} {:>10.2f} {:>8.1f}\n", kind.name,
                       t.apply / r, t.preview / r, static_cast<double>(t.preview_layouts) / r,
                       t.full / r, static_cast<double>(t.full_layouts) / r);
  }
  out << fmt::format("result      {}\n", ok ? "ok" : "errors");
  return ok ? 0 : 1;
}

}  // namespace vs::cli

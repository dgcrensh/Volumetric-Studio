// Decision 0029: an engine that reuses layouts (the default) gives exactly
// what one that builds and checks everything every time gives, edit after
// edit, while building fewer layouts.

#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "bracket.hpp"
#include "nested.hpp"
#include "scenes.hpp"
#include "vs/eval/cache.hpp"
#include "vs/eval/engine.hpp"
#include "vs/feature/tree.hpp"
#include "vs/kernel/exchange.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"

using namespace vs;
using core::Id;
using eval::Engine;
using eval::EngineOptions;
using feature::Param;

namespace {

const core::ProductSettings kSettings = core::ProductSettings::defaults();

std::string text(const core::Diagnostics& ds) {
  std::string s;
  for (const auto& d : ds) {
    s += fmt::format("{} {} {}", static_cast<int>(d.severity), d.code, d.message);
    for (const auto& sub : d.subjects) s += " " + sub.id.to_string() + "/" + sub.label;
    s += "\n";
  }
  return s;
}

// Everything an evaluation shows, as text: equal text, equal evaluations.
std::string describe(const eval::Evaluation& ev) {
  std::string s = fmt::format("checked {} cancelled {} waves {} output {}\n", ev.checked,
                              ev.cancelled, ev.stats.waves, ev.output_hash().to_hex());
  s += "diagnostics\n" + text(ev.diagnostics);
  s += "layout\n" + text(ev.layout.diagnostics);
  for (std::size_t i = 0; i < ev.volumes.size(); ++i) {
    const auto& r = ev.volumes[i];
    s += fmt::format("volume {} {} key {} stale {} regenerated {} cache {}\n", r.volume.to_string(),
                     static_cast<int>(r.status), r.key ? r.key->to_hex() : "-", r.stale_contents,
                     r.regenerated, r.from_cache);
    s += text(r.diagnostics);
  }
  for (const auto& vb : ev.build.volumes) {
    s += fmt::format("build {} {} boundary {}\n", vb.volume.to_string(), static_cast<int>(vb.status),
                     vb.boundary.is_empty() ? "-" : kernel::shape_hash(vb.boundary).to_hex());
    for (const auto& f : vb.boundary_faces) s += " face " + f.name;
    for (const auto& [name, value] : vb.parameters) s += fmt::format(" {}={}", name, value);
    s += "\n" + text(vb.diagnostics);
  }
  for (const auto& cb : ev.build.connections) {
    s += fmt::format("surface {} ok {} patch {}\n", cb.surface.to_string(), cb.ok,
                     cb.ok ? kernel::shape_hash(cb.patch).to_hex() : "-");
  }
  s += "build diagnostics\n" + text(ev.build.diagnostics);
  return s;
}

struct Pair {
  Engine reusing{kSettings, options(true)};
  Engine building{kSettings, options(false)};
  std::size_t reused = 0;
  std::size_t built = 0;  // by the reusing engine, in the last evaluate()

  static EngineOptions options(bool reuse) {
    EngineOptions o;
    o.threads = 2;
    o.reuse_layouts = reuse;
    return o;
  }

  // The preview tier, then the full evaluation, on both engines.
  void evaluate(const model::Project& project, const std::string& step) {
    INFO(step);
    built = 0;
    for (const bool full : {false, true}) {
      eval::EvaluateOptions eo;
      eo.generate = full;
      const auto a = reusing.evaluate(project, eo);
      const auto b = building.evaluate(project, eo);
      CHECK(a.document == b.document);
      CHECK(describe(a) == describe(b));
      CHECK(a.stats.layout_builds + a.stats.layouts_reused == b.stats.layout_builds);
      CHECK(b.stats.layouts_reused == 0);
      reused += a.stats.layouts_reused;
      built += a.stats.layout_builds;
    }
  }
};

void set_depth_expression(model::Project& p, const Id& volume, std::string expression) {
  auto* tree = p.document.find_volume(volume)->tree();
  REQUIRE(tree != nullptr);
  auto& f = *tree->find(tree->features().back().id);
  std::get<feature::ExtrudeFeature>(f.definition).depth = Param::expr(std::move(expression));
}

void set_parameter_expression(model::Project& p, const Id& volume, const Id& parameter,
                              std::string expression) {
  for (auto& q : p.document.find_volume(volume)->parameters) {
    if (q.id == parameter) q.value = Param::expr(std::move(expression));
  }
}

}  // namespace

TEST_CASE("Reused layouts give what building them gives: a chain of links", "[eval][reuse]") {
  scenes::Row r = scenes::chain();
  model::Project& p = r.project;
  Pair pair;
  pair.evaluate(p, "first");
  pair.evaluate(p, "again");
  CHECK(pair.reused > 0);

  // C's contents: the layout is unchanged.
  set_depth_expression(p, r.volumes[2], "depth - 1");
  pair.evaluate(p, "C's contents");
  CHECK(pair.built == 0);
  // C's parameter: only C's parameters differ.
  scenes::set_parameter(p, r.volumes[2], r.depth[2], 7);
  pair.evaluate(p, "C's depth");
  CHECK(pair.built == 0);  // refreshed
  // A's parameter: its output changes, and with it B's linked feed.
  scenes::set_parameter(p, r.volumes[0], r.depth[0], 6);
  pair.evaluate(p, "A's depth");
  scenes::set_parameter(p, r.volumes[0], r.depth[0], 10);
  pair.evaluate(p, "A's depth back");
  // A parameter that fails, then one that refers to nothing, then fixed:
  // these can't be refreshed and are built.
  scenes::set_parameter(p, r.volumes[2], r.depth[2], std::numeric_limits<double>::infinity());
  pair.evaluate(p, "C's depth not finite");
  CHECK(pair.built == 1);
  scenes::set_parameter(p, r.volumes[2], r.depth[2], 8);
  pair.evaluate(p, "C's depth fixed");
  CHECK(pair.built == 1);
  // A boundary moves: a new layout.
  auto& layout = p.document.layout;
  auto& point = *layout.find(layout.features()[5].id);  // C's max corner
  std::get<feature::DatumPointFeature>(point.definition).definition =
      feature::PointAt{Param::of(70), Param::of(20), Param::of(25)};
  pair.evaluate(p, "C's box");
}

TEST_CASE("Reused layouts give what building them gives: the bracket", "[eval][reuse]") {
  bracket::Bracket b = bracket::make_bracket();
  model::Project& p = b.project;
  Pair pair;
  pair.evaluate(p, "first");
  // Base's contents, its thickness, then a layout variable.
  set_depth_expression(p, b.base, "thickness - 1");
  pair.evaluate(p, "Base's contents");
  for (auto& q : p.document.find_volume(b.base)->parameters) {
    if (q.id == b.base_thickness) q.value = Param::of(8);
  }
  pair.evaluate(p, "Base's thickness");
  CHECK(pair.built == 0);
  REQUIRE_FALSE(p.document.layout.variables().set_expression(b.base_h, "12").has_errors());
  pair.evaluate(p, "base_h");
  // Upright's load is a link target: an authored value gives way to the link.
  set_parameter_expression(p, b.upright, b.upright_load, "1");
  pair.evaluate(p, "Upright's load");
  CHECK(pair.reused > 0);
}

TEST_CASE("Reused layouts give what building them gives: nested composites", "[eval][reuse]") {
  nested::Nested n = nested::make_nested();
  nested::fill_leaves(n);
  model::Project& p = n.project;
  Pair pair;
  pair.evaluate(p, "first");
  pair.evaluate(p, "again");
  // A composite's parameter changes its layout: built, never refreshed.
  for (auto& q : p.document.find_volume(n.housing)->parameters) {
    if (q.id == n.width) q.value = Param::of(36);
  }
  pair.evaluate(p, "Housing's width");
  REQUIRE_FALSE(p.document.layout.variables().set_expression(n.w, "36").has_errors());
  pair.evaluate(p, "w");
  CHECK(pair.reused > 0);
}

TEST_CASE("A layout depends only on its layout inputs", "[eval][reuse]") {
  for (const auto& [name, project] :
       std::vector<std::pair<std::string, model::Project>>{
           {"chain", scenes::chain().project},
           {"bracket", bracket::make_bracket().project},
           {"nested", [] {
              auto n = nested::make_nested();
              nested::fill_leaves(n);
              return n.project;
            }()}}) {
    INFO(name);
    const model::Document& doc = project.document;
    const model::Document inputs = model::layout_inputs(doc);
    const auto a = model::build_layout(doc, kSettings);
    const auto b = model::build_layout(inputs, kSettings);
    CHECK(text(a.diagnostics) == text(b.diagnostics));
    REQUIRE(a.volumes.size() == b.volumes.size());
    for (std::size_t i = 0; i < a.volumes.size(); ++i) {
      CHECK(kernel::shape_hash(a.volumes[i].boundary) == kernel::shape_hash(b.volumes[i].boundary));
      CHECK(a.volumes[i].parameters == b.volumes[i].parameters);
      CHECK(eval::cache_key(doc, a, i, kSettings) == eval::cache_key(doc, b, i, kSettings));
    }
    CHECK(text(validate::validate_layout(doc, a, kSettings).diagnostics) ==
          text(validate::validate_layout(inputs, b, kSettings).diagnostics));
    // Layout inputs drop what only contents read, and nothing a layout reads.
    CHECK(model::layout_inputs(inputs) == inputs);
    CHECK(model::layout_shape(doc) == model::layout_shape(inputs));
  }
}

TEST_CASE("refresh_parameters evaluates a leaf's parameters as build_layout does",
          "[eval][reuse]") {
  bracket::Bracket b = bracket::make_bracket();
  model::Document doc = b.project.document;
  const auto before = model::build_layout(doc, kSettings);
  const auto base = static_cast<std::size_t>(doc.find_volume(b.base) - doc.volumes.data());

  for (auto& q : doc.find_volume(b.base)->parameters) {
    if (q.id == b.base_thickness) q.value = Param::expr("base_h - 3");  // a layout variable
  }
  REQUIRE(model::layout_shape(doc) == model::layout_shape(b.project.document));
  model::DocumentBuild refreshed = before;
  REQUIRE(model::refresh_parameters(doc, refreshed, base, kSettings));
  const auto built = model::build_layout(doc, kSettings);
  CHECK(refreshed.volumes[base].parameters == built.volumes[base].parameters);
  CHECK(refreshed.volumes[base].parameters.at("thickness") == 7.0);
  CHECK(text(refreshed.diagnostics) == text(built.diagnostics));

  // A parameter that doesn't evaluate is left to build_layout().
  for (auto& q : doc.find_volume(b.base)->parameters) {
    if (q.id == b.base_thickness) q.value = Param::expr("no_such_name");
  }
  model::DocumentBuild untouched = before;
  CHECK_FALSE(model::refresh_parameters(doc, untouched, base, kSettings));
  CHECK(untouched.volumes[base].parameters == before.volumes[base].parameters);
  // ... and so is a layout where one didn't.
  const auto failed = model::build_layout(doc, kSettings);
  for (auto& q : doc.find_volume(b.base)->parameters) {
    if (q.id == b.base_thickness) q.value = Param::of(6);
  }
  model::DocumentBuild from_failed = failed;
  CHECK_FALSE(model::refresh_parameters(doc, from_failed, base, kSettings));
}

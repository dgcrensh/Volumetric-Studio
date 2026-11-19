#include "scenes.hpp"

#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "vs/feature/tree.hpp"
#include "vs/model/check.hpp"

namespace scenes {

using namespace vs::model;
using vs::core::dvec2;
using vs::core::dvec3;
using vs::feature::DatumPointFeature;
using vs::feature::ExtrudeFeature;
using vs::feature::FaceRef;
using vs::feature::FeatureRef;
using vs::feature::Param;
using vs::feature::PointAt;
using vs::feature::SketchFeature;
using vs::sketch::Sketch;

namespace {

// A fully constrained axis-aligned rectangle from `lo` to `hi`.
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

Id point(Document& doc, const std::string& name, dvec3 p) {
  return doc.layout.add(DatumPointFeature{PointAt{Param::of(p.x), Param::of(p.y), Param::of(p.z)}},
                        name);
}

}  // namespace

Row row(std::size_t n) {
  Row r;
  Document& doc = r.project.document;
  doc = make_document("Row");
  for (std::size_t i = 0; i < n; ++i) {
    const double x0 = 25.0 * static_cast<double>(i);
    const std::string name = fmt::format("V{}", i);
    Volume v;
    v.id = Id::random();
    v.name = name;
    v.boundary = BoxBoundary{FeatureRef{point(doc, name + " min", {x0, 0, 0})},
                             FeatureRef{point(doc, name + " max", {x0 + 20, 20, 20})}};
    const Id depth = Id::random();
    v.parameters.push_back({depth, "depth", Param::of(10)});
    const Id out = Id::random();
    v.outputs.push_back({out, "volume", OutputQuantity::Volume, 1.0});
    v.seed = i;

    ParametricContents contents;
    Sketch sketch(Id::random(), "Outline");
    // The "+z" face's frame has x and y along the model's x and y.
    rectangle(sketch, {x0 + 2, 2}, {x0 + 18, 18});
    const Id s = contents.tree.add(SketchFeature{sketch, FaceRef{v.id, "+z"}});
    ExtrudeFeature fill;
    fill.sketch = s;
    fill.depth = Param::expr("depth");
    fill.reverse = true;  // down, into the box
    contents.tree.add(fill, "Fill");
    v.contents = std::move(contents);

    r.volumes.push_back(v.id);
    r.depth.push_back(depth);
    r.output.push_back(out);
    doc.volumes.push_back(std::move(v));
  }
  REQUIRE_FALSE(check(doc).has_errors());
  return r;
}

Row chain() {
  Row r = row(3);
  Document& doc = r.project.document;
  const char* names[] = {"A", "B", "C"};
  for (std::size_t i = 0; i < 3; ++i) doc.volumes[i].name = names[i];
  Volume& b = doc.volumes[1];
  b.parameters.front().name = "feed";
  b.parameters.front().value = Param::of(2560);
  auto& tree = std::get<ParametricContents>(b.contents).tree;
  auto* feature = tree.find(tree.features().back().id);
  REQUIRE(feature != nullptr);
  std::get<ExtrudeFeature>(feature->definition).depth = Param::expr("feed / 256");
  doc.links.push_back({Id::random(), {r.volumes[0], r.output[0]}, r.depth[1]});
  REQUIRE_FALSE(check(doc).has_errors());
  return r;
}

void set_parameter(Project& project, const Id& volume, const Id& parameter, double value) {
  Volume* v = project.document.find_volume(volume);
  REQUIRE(v != nullptr);
  for (auto& p : v->parameters) {
    if (p.id == parameter) {
      p.value = Param::of(value);
      return;
    }
  }
  FAIL("no such parameter");
}

TempDir::TempDir() {
  const auto p = std::filesystem::temp_directory_path() / ("vs_eval_" + Id::random().to_string());
  std::filesystem::create_directories(p);
  path = p.string();
}

TempDir::~TempDir() {
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
}

}  // namespace scenes

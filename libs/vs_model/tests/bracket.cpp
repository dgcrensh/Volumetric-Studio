#include "bracket.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "vs/core/settings.hpp"
#include "vs/kernel/exchange.hpp"
#include "vs/kernel/primitives.hpp"

namespace bracket {

using namespace vs::model;
using vs::core::dvec2;
using vs::feature::DatumPlaneFeature;
using vs::feature::ExtrudeFeature;
using vs::feature::FeatureRef;
using vs::feature::PlaneOffset;
using vs::feature::PrincipalPlane;
using vs::feature::SketchFeature;
using vs::sketch::Sketch;

namespace {

void set_expression(Sketch& sketch, const Id& constraint, const std::string& expression) {
  auto* c = sketch.find_constraint(constraint);
  REQUIRE(c != nullptr);
  REQUIRE(c->dimension.has_value());
  c->dimension->expression = expression;
}

// A fully constrained axis-aligned rectangle from `lo` to `hi`, its width and
// height driven by expressions when given.
void rectangle(Sketch& sketch, dvec2 lo, dvec2 hi, const std::string& width = {},
               const std::string& height = {}) {
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
  const Id cw = sketch.length(l[0], hi.x - lo.x);
  const Id ch = sketch.length(l[1], hi.y - lo.y);
  if (!width.empty()) set_expression(sketch, cw, width);
  if (!height.empty()) set_expression(sketch, ch, height);
}

}  // namespace

vs::model::Blob step_box(double x0, double y0, double z0, double x1, double y1, double z1) {
  const auto settings = vs::core::ProductSettings::defaults();
  auto box = vs::kernel::make_box(vs::core::dbox3::from_min_max({x0, y0, z0}, {x1, y1, z1}),
                                  settings);
  REQUIRE(box.ok());
  const auto path = std::filesystem::temp_directory_path() /
                    ("vs_model_box_" + Id::random().to_string() + ".step");
  REQUIRE_FALSE(vs::kernel::write_step(*box, path).has_errors());
  std::ifstream in(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  std::filesystem::remove(path);
  vs::model::Blob out(text.size());
  std::transform(text.begin(), text.end(), out.begin(),
                 [](char c) { return static_cast<std::byte>(c); });
  return out;
}

Bracket make_bracket() {
  Bracket b;
  Document& doc = b.project.document;
  doc = make_document("Bracket");

  // --- layout
  auto& vars = doc.layout.variables();
  b.width = vars.add("width", "60 mm").value();
  b.depth = vars.add("depth", "40 mm").value();
  b.base_h = vars.add("base_h", "10 mm").value();
  b.upright_t = vars.add("upright_t", "10 mm").value();
  b.upright_h = vars.add("upright_h", "40 mm").value();

  Sketch base(Id::random(), "Base outline");
  rectangle(base, {0, 0}, {60, 40}, "width", "depth");
  b.base_sketch = doc.layout.add(SketchFeature{base, PrincipalPlane::XY});
  ExtrudeFeature base_block;
  base_block.sketch = b.base_sketch;
  base_block.depth = vs::feature::Param::expr("base_h");
  b.base_block = doc.layout.add(base_block, "Base block");

  b.top_plane = doc.layout.add(
      DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, vs::feature::Param::expr("base_h")}},
      "Top of base");

  Sketch upright(Id::random(), "Upright outline");
  rectangle(upright, {0, 0}, {60, 10}, "width", "upright_t");
  b.upright_sketch = doc.layout.add(SketchFeature{upright, FeatureRef{b.top_plane}});
  ExtrudeFeature upright_block;
  upright_block.sketch = b.upright_sketch;
  upright_block.depth = vs::feature::Param::expr("upright_h");
  b.upright_block = doc.layout.add(upright_block, "Upright block");

  Sketch flange(Id::random(), "Flange outline");
  rectangle(flange, {0, 0}, {60, 10}, "width", "upright_t");
  b.flange_sketch = doc.layout.add(SketchFeature{flange, FeatureRef{b.top_plane}});

  // --- Base: parametric
  Volume base_volume;
  base_volume.id = b.base = Id::random();
  base_volume.name = "Base";
  base_volume.boundary = FeatureBoundary{{b.base_block}};
  b.base_thickness = Id::random();
  base_volume.parameters.push_back(
      {b.base_thickness, "thickness", vs::feature::Param::expr("base_h")});
  b.base_mass = Id::random();
  b.base_volume = Id::random();
  base_volume.outputs.push_back({b.base_mass, "mass", OutputQuantity::Mass, 1.24});
  base_volume.outputs.push_back({b.base_volume, "volume", OutputQuantity::Volume, 1.0});
  base_volume.seed = 42;

  // --- the flange, needed by Base's contents
  ConnectionSurface fl;
  fl.id = b.flange = Id::random();
  fl.name = "Flange";
  fl.geometry = SketchPatch{b.flange_sketch};
  BoltedFlangeParams bolts;
  BoltGrid grid;
  grid.columns = vs::feature::Param::of(2);
  grid.rows = vs::feature::Param::of(1);
  grid.pitch_x = vs::feature::Param::of(40);
  bolts.holes.layout = grid;
  bolts.holes.u = vs::feature::Param::expr("width / 2");
  bolts.holes.v = vs::feature::Param::expr("upright_t / 2");
  bolts.thread = "M4";
  bolts.min_wall = vs::feature::Param::of(2);
  fl.params = bolts;

  {
    ParametricContents contents;
    Sketch inner(Id::random(), "Fill outline");
    rectangle(inner, {0, 0}, {60, 40}, "60", "40");
    b.inner_sketch = contents.tree.add(SketchFeature{inner, FeatureRef{b.flange}});
    ExtrudeFeature fill;
    fill.sketch = b.inner_sketch;
    fill.depth = vs::feature::Param::expr("thickness");
    fill.reverse = true;  // down into the base, away from the flange
    b.inner_extrude = contents.tree.add(fill, "Fill");
    base_volume.contents = std::move(contents);
  }

  // --- Upright: static, a STEP file of the same box
  Volume upright_volume;
  upright_volume.id = b.upright = Id::random();
  upright_volume.name = "Upright";
  upright_volume.boundary = FeatureBoundary{{b.upright_block}};
  const std::string blob = b.project.add_blob(step_box(0, 0, 10, 60, 10, 50), "step");
  upright_volume.contents = StaticContents{blob, "step"};
  b.upright_load = Id::random();
  upright_volume.parameters.push_back({b.upright_load, "load", vs::feature::Param::of(0)});
  b.upright_envelope = Id::random();
  upright_volume.outputs.push_back(
      {b.upright_envelope, "envelope", OutputQuantity::Envelope, 1.0});

  fl.volumes = {b.base, b.upright};
  doc.volumes.push_back(std::move(base_volume));
  doc.volumes.push_back(std::move(upright_volume));
  doc.connections.push_back(std::move(fl));

  b.link = Id::random();
  doc.links.push_back({b.link, {b.base, b.base_mass}, b.upright_load});
  return b;
}

std::string codes(const vs::core::Diagnostics& diagnostics) {
  std::ostringstream s;
  for (const auto& d : diagnostics) s << d.code << " ";
  return s.str();
}

}  // namespace bracket

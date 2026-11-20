#include "curved.hpp"

#include <numbers>
#include <string>

namespace curved {

using namespace vs::model;
using vs::feature::ExtrudeFeature;
using vs::feature::Param;
using vs::feature::PrincipalAxis;
using vs::feature::PrincipalPlane;
using vs::feature::SketchFeature;
using vs::sketch::Sketch;

namespace {

constexpr double kPi = std::numbers::pi;

// A half disc of radius r (y >= 0), or a half ring from r to R.
void half_profile(Sketch& s, double r, double big_r) {
  const Id c = s.add_point({0, 0});
  if (big_r <= 0) {
    const Id a = s.add_point({r, 0});
    const Id b = s.add_point({-r, 0});
    s.add_arc(c, a, b);
    s.add_line(b, a);
    return;
  }
  const Id a = s.add_point({r, 0});
  const Id b = s.add_point({-r, 0});
  const Id big_a = s.add_point({big_r, 0});
  const Id big_b = s.add_point({-big_r, 0});
  s.add_arc(c, big_a, big_b);
  s.add_line(big_b, b);
  s.add_arc(c, a, b);
  s.add_line(a, big_a);
}

}  // namespace

static Sketch profile(const Options& o, bool shaft_side, const std::string& name) {
  Sketch s(Id::random(), name);
  const double bore = o.radius + o.gap;
  if (o.half) {
    if (shaft_side) {
      half_profile(s, o.radius, 0);
    } else {
      half_profile(s, bore, o.outer);
    }
  } else if (shaft_side) {
    s.add_circle(s.add_point({0, 0}), o.radius);
  } else {
    const Id c = s.add_point({0, 0});
    s.add_circle(c, o.outer);
    s.add_circle(c, bore);
  }
  return s;
}

Curved make_curved(const Options& o) {
  Curved out;
  Document& doc = out.project.document;
  doc = make_document(o.half ? "Half shell" : "Shaft in sleeve");
  const Sketch shaft = profile(o, true, "Shaft outline");
  const Sketch sleeve = profile(o, false, "Sleeve outline");
  out.shaft_sketch = doc.layout.add(SketchFeature{shaft, PrincipalPlane::XY});
  out.sleeve_sketch = doc.layout.add(SketchFeature{sleeve, PrincipalPlane::XY});
  ExtrudeFeature shaft_block;
  shaft_block.sketch = out.shaft_sketch;
  shaft_block.depth = Param::of(o.height);
  out.shaft_block = doc.layout.add(shaft_block, "Shaft block");
  ExtrudeFeature sleeve_block;
  sleeve_block.sketch = out.sleeve_sketch;
  sleeve_block.depth = Param::of(o.height);
  out.sleeve_block = doc.layout.add(sleeve_block, "Sleeve block");

  Volume shaft_volume;
  shaft_volume.id = out.shaft = Id::random();
  shaft_volume.name = "Shaft";
  shaft_volume.boundary = FeatureBoundary{{out.shaft_block}};
  Volume sleeve_volume;
  sleeve_volume.id = out.sleeve = Id::random();
  sleeve_volume.name = "Sleeve";
  sleeve_volume.boundary = FeatureBoundary{{out.sleeve_block}};

  ConnectionSurface s;
  s.id = out.surface = Id::random();
  s.name = o.half ? "Half bore" : "Bore";
  CylinderPatch patch;
  patch.axis = PrincipalAxis::Z;
  patch.radius = Param::of(o.radius);
  patch.start_angle = Param::of(0);
  patch.sweep = Param::of(o.half ? kPi : 2 * kPi);
  patch.start = Param::of(o.patch_start);
  patch.length = Param::of(o.patch_length);
  s.geometry = patch;
  s.params = o.params;
  s.volumes = {out.shaft, out.sleeve};

  doc.volumes.push_back(std::move(shaft_volume));
  doc.volumes.push_back(std::move(sleeve_volume));
  doc.connections.push_back(std::move(s));
  return out;
}

ParametricContents fill(const Options& o, bool shaft) {
  ParametricContents contents;
  const Id s = contents.tree.add(
      SketchFeature{profile(o, shaft, shaft ? "Shaft fill" : "Sleeve fill"), PrincipalPlane::XY});
  ExtrudeFeature e;
  e.sketch = s;
  e.depth = Param::of(o.height);
  contents.tree.add(e, "Fill");
  return contents;
}

}  // namespace curved

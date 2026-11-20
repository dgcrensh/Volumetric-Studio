#include "rack.hpp"

#include <utility>

#include "nested.hpp"
#include "vs/feature/tree.hpp"

namespace rack {

using namespace vs::model;
using vs::core::dbox3;
using vs::core::dvec3;
using vs::feature::DatumPointFeature;
using vs::feature::ExtrudeFeature;
using vs::feature::FaceRef;
using vs::feature::FeatureRef;
using vs::feature::FeatureTree;
using vs::feature::Param;
using vs::feature::PointAt;
using vs::feature::SketchFeature;
using vs::sketch::Sketch;

namespace {

Id point(FeatureTree& tree, double x, double y, double z, std::string name) {
  return tree.add(DatumPointFeature{PointAt{Param::of(x), Param::of(y), Param::of(z)}},
                  std::move(name));
}

}  // namespace

ParametricContents cell_tree(const Id& template_id, const dvec3& size) {
  ParametricContents contents;
  FeatureTree& t = contents.tree;
  // The "+z" face's frame has x and y along the model's (frame_for()).
  Sketch s(Id::random(), "Outline");
  const Id a = s.add_point({0, 0});
  const Id b = s.add_point({size.x, 0});
  const Id c = s.add_point({size.x, size.y});
  const Id d = s.add_point({0, size.y});
  s.add_line(a, b);
  s.add_line(b, c);
  s.add_line(c, d);
  s.add_line(d, a);
  const Id sketch = t.add(SketchFeature{s, FaceRef{template_id, "+z"}}, "Outline");
  ExtrudeFeature e;
  e.sketch = sketch;
  e.depth = Param::expr("depth");
  e.reverse = true;  // down, into the cell
  t.add(e, "Fill");
  return contents;
}

Rack make_rack(std::string generator) {
  Rack r;
  Document& doc = r.project.document;
  doc = make_document("Rack");
  const Id base_lo = point(doc.layout, 0, 0, -10, "Base low");
  const Id origin = point(doc.layout, 0, 0, 0, "Origin");
  const Id base_hi = point(doc.layout, 60, 40, 0, "Base high");
  const Id rack_hi = point(doc.layout, 60, 40, 10, "Rack high");

  Volume base;
  base.id = Id::random();
  base.name = "Base";
  base.boundary = BoxBoundary{FeatureRef{base_lo}, FeatureRef{base_hi}};
  base.contents = nested::fill(dbox3::from_min_max({0, 0, -10}, {60, 40, 0}));
  r.base = base.id;

  Volume rack;
  rack.id = Id::random();
  rack.name = "Rack";
  rack.boundary = BoxBoundary{FeatureRef{origin}, FeatureRef{rack_hi}};
  CompositeContents contents;
  contents.generator = std::move(generator);
  ChildTemplate cell;
  cell.id = Id::random();
  cell.name = "Cell";
  cell.contents = cell_tree(cell.id, {20, 20, 10});
  cell.parameters.push_back({Id::random(), "depth", Param::of(10)});
  cell.outputs.push_back({Id::random(), "volume", OutputQuantity::Volume, 1.0});
  r.cell = cell.id;
  r.depth = cell.parameters.front().id;
  contents.templates.push_back(std::move(cell));
  rack.contents = std::move(contents);
  rack.outputs.push_back({Id::random(), "volume", OutputQuantity::Volume, 1.0});
  r.rack = rack.id;

  DatumPatch seat;
  seat.plane = vs::feature::PrincipalPlane::XY;
  seat.u = Param::of(10);
  seat.v = Param::of(10);
  seat.width = Param::of(16);
  seat.height = Param::of(16);
  ConnectionSurface s;
  s.id = Id::random();
  s.name = "Seat";
  s.geometry = seat;
  s.params = BondedParams{};
  s.volumes = {r.base, r.rack};
  r.seat = s.id;

  doc.volumes = {std::move(base), std::move(rack)};
  doc.connections = {std::move(s)};
  return r;
}

}  // namespace rack

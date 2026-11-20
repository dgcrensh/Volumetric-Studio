#include "nested.hpp"

#include <string>

namespace nested {

using namespace vs::model;
using vs::core::dbox3;
using vs::core::dvec3;
using vs::feature::DatumPlaneFeature;
using vs::feature::DatumPointFeature;
using vs::feature::ExtrudeFeature;
using vs::feature::FeatureRef;
using vs::feature::FeatureTree;
using vs::feature::Param;
using vs::feature::PlaneOffset;
using vs::feature::PointAt;
using vs::feature::PrincipalPlane;
using vs::feature::SketchFeature;
using vs::sketch::Sketch;

namespace {

Id point(FeatureTree& tree, Param x, Param y, Param z, std::string name) {
  return tree.add(DatumPointFeature{PointAt{std::move(x), std::move(y), std::move(z)}},
                  std::move(name));
}

Id point(FeatureTree& tree, double x, double y, double z, std::string name) {
  return point(tree, Param::of(x), Param::of(y), Param::of(z), std::move(name));
}

Volume box_volume(std::string name, const Id& parent, const Id& lo, const Id& hi) {
  Volume v;
  v.id = Id::random();
  v.name = std::move(name);
  v.parent = parent;
  v.boundary = BoxBoundary{FeatureRef{lo}, FeatureRef{hi}};
  return v;
}

ConnectionSurface bonded(std::string name, DatumPatch patch, std::vector<Id> volumes) {
  ConnectionSurface s;
  s.id = Id::random();
  s.name = std::move(name);
  s.geometry = std::move(patch);
  s.params = BondedParams{};
  s.volumes = std::move(volumes);
  return s;
}

}  // namespace

Nested make_nested() {
  Nested n;
  Document& doc = n.project.document;
  doc = make_document("Nested");
  n.w = *doc.layout.variables().add("w", "40");

  // The root level.
  const Id base_lo = point(doc.layout, 0, 0, -10, "Base low");
  const Id origin = point(doc.layout, 0, 0, 0, "Origin");
  const Id base_hi = point(doc.layout, Param::expr("w"), Param::of(20), Param::of(0), "Base high");
  const Id housing_hi =
      point(doc.layout, Param::expr("w"), Param::of(20), Param::of(20), "Housing high");

  Volume base = box_volume("Base", {}, base_lo, base_hi);
  n.base = base.id;

  // Housing: a composite whose layout splits it into Left and Right.
  Volume housing = box_volume("Housing", {}, origin, housing_hi);
  n.housing = housing.id;
  n.width = Id::random();
  housing.parameters.push_back({n.width, "width", Param::expr("w")});
  CompositeContents housing_contents;
  FeatureTree& hl = housing_contents.layout;
  const Id h_origin = point(hl, 0, 0, 0, "Origin");
  n.housing_split = point(hl, 20, 20, 20, "Split");
  const Id h_right_lo = point(hl, 20, 0, 0, "Right low");
  const Id h_right_hi =
      point(hl, Param::expr("width"), Param::of(20), Param::of(20), "Right high");
  const Id h_plane = hl.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::YZ, Param::of(20)}},
                            "Split plane");

  Volume left = box_volume("Left", n.housing, h_origin, n.housing_split);
  n.left = left.id;
  Volume right = box_volume("Right", n.housing, h_right_lo, h_right_hi);
  n.right = right.id;

  // Left: a composite of Core and Cap.
  CompositeContents left_contents;
  FeatureTree& ll = left_contents.layout;
  const Id l_origin = point(ll, 0, 0, 0, "Origin");
  n.left_mid = point(ll, 20, 20, 10, "Middle");
  const Id l_cap_lo = point(ll, 0, 0, 10, "Cap low");
  const Id l_top = point(ll, 20, 20, 20, "Top");
  const Id l_plane =
      ll.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(10)}}, "Joint plane");

  Volume core = box_volume("Core", n.left, l_origin, n.left_mid);
  n.core = core.id;
  Volume cap = box_volume("Cap", n.left, l_cap_lo, l_top);
  n.cap = cap.id;

  // Surfaces, each at its level.
  DatumPatch seat;
  seat.plane = PrincipalPlane::XY;
  seat.u = Param::of(10);
  seat.v = Param::of(10);
  seat.width = Param::of(20);
  seat.height = Param::of(20);
  ConnectionSurface seat_surface = bonded("Seat", seat, {n.base, n.housing});
  n.seat = seat_surface.id;

  DatumPatch middle;  // on x = 20: u along y, v along z
  middle.plane = FeatureRef{h_plane};
  middle.u = Param::of(10);
  middle.v = Param::of(5);
  middle.width = Param::of(20);
  middle.height = Param::of(10);
  ConnectionSurface middle_surface = bonded("Middle", middle, {n.left, n.right});
  n.middle = middle_surface.id;

  DatumPatch joint;
  joint.plane = FeatureRef{l_plane};
  joint.u = Param::of(10);
  joint.v = Param::of(10);
  joint.width = Param::of(20);
  joint.height = Param::of(20);
  ConnectionSurface joint_surface = bonded("Joint", joint, {n.core, n.cap});
  n.joint = joint_surface.id;

  housing_contents.delegations.push_back({n.seat, n.left});
  left_contents.delegations.push_back({n.seat, n.core});
  left_contents.delegations.push_back({n.middle, n.core});
  housing.contents = std::move(housing_contents);
  left.contents = std::move(left_contents);

  doc.volumes = {std::move(base), std::move(housing), std::move(left),
                 std::move(right), std::move(core),   std::move(cap)};
  doc.connections = {std::move(seat_surface), std::move(middle_surface), std::move(joint_surface)};
  return n;
}

dbox3 box_of(const Nested& n, const Id& volume) {
  if (volume == n.base) return dbox3::from_min_max({0, 0, -10}, {40, 20, 0});
  if (volume == n.housing) return dbox3::from_min_max({0, 0, 0}, {40, 20, 20});
  if (volume == n.left) return dbox3::from_min_max({0, 0, 0}, {20, 20, 20});
  if (volume == n.right) return dbox3::from_min_max({20, 0, 0}, {40, 20, 20});
  if (volume == n.core) return dbox3::from_min_max({0, 0, 0}, {20, 20, 10});
  return dbox3::from_min_max({0, 0, 10}, {20, 20, 20});  // cap
}

ParametricContents fill(const dbox3& box) {
  ParametricContents contents;
  FeatureTree& t = contents.tree;
  const Id plane =
      t.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(box.min.z)}}, "Bottom");
  Sketch s(Id::random(), "Outline");
  const Id a = s.add_point({box.min.x, box.min.y});
  const Id b = s.add_point({box.max.x, box.min.y});
  const Id c = s.add_point({box.max.x, box.max.y});
  const Id d = s.add_point({box.min.x, box.max.y});
  s.add_line(a, b);
  s.add_line(b, c);
  s.add_line(c, d);
  s.add_line(d, a);
  const Id sketch = t.add(SketchFeature{s, FeatureRef{plane}}, "Outline");
  ExtrudeFeature e;
  e.sketch = sketch;
  e.depth = Param::of(box.max.z - box.min.z);
  t.add(e, "Fill");
  return contents;
}

void fill_leaves(Nested& n) {
  for (auto& v : n.project.document.volumes) {
    if (!v.composite()) v.contents = fill(box_of(n, v.id));
  }
}

}  // namespace nested

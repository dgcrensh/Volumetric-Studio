#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "parts.hpp"
#include "vs/feature/rebuild.hpp"
#include "vs/kernel/queries.hpp"

using namespace vs::feature;
using Catch::Approx;
using parts::kSettings;
using vs::core::dvec2;

namespace {

[[maybe_unused]] constexpr double kPi = std::numbers::pi;

void check_vec(const dvec3& got, const dvec3& want, double margin = 1e-9) {
  INFO("got (" << got.x << ", " << got.y << ", " << got.z << "), want (" << want.x << ", "
               << want.y << ", " << want.z << ")");
  CHECK(got.x == Approx(want.x).margin(margin));
  CHECK(got.y == Approx(want.y).margin(margin));
  CHECK(got.z == Approx(want.z).margin(margin));
}

const FeatureResult& ok_result(const RebuildResult& r, const Id& id) {
  const FeatureResult* f = r.find(id);
  REQUIRE(f != nullptr);
  INFO(parts::codes(f->diagnostics));
  REQUIRE(f->ok());
  return *f;
}

}  // namespace

TEST_CASE("principal planes and the frame rule for other planes", "[feature][datum]") {
  const auto xy = principal_frame(PrincipalPlane::XY);
  const auto yz = principal_frame(PrincipalPlane::YZ);
  const auto zx = principal_frame(PrincipalPlane::ZX);
  check_vec(xy.normal(), {0, 0, 1});
  check_vec(yz.normal(), {1, 0, 0});
  check_vec(zx.normal(), {0, 1, 0});

  // A plane z = 5 facing down: origin above the model origin, x along X.
  const auto down = frame_for({{3, 4, 5}, {0, 0, -1}});
  check_vec(down.origin, {0, 0, 5});
  check_vec(down.x_axis, {1, 0, 0});
  check_vec(down.normal(), {0, 0, -1});
  // A plane facing +X takes Y as its x axis.
  const auto side = frame_for({{7, 1, 1}, {1, 0, 0}});
  check_vec(side.origin, {7, 0, 0});
  check_vec(side.x_axis, {0, 1, 0});
  check_vec(side.normal(), {1, 0, 0});
}

TEST_CASE("datum planes: offset, through three points, through an axis at an angle",
          "[feature][datum]") {
  FeatureTree tree;
  REQUIRE(tree.variables().add("gap", "12.5").ok());
  const Id offset =
      tree.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::YZ, Param::expr("gap * 2")}});
  const Id offset2 = tree.add(DatumPlaneFeature{PlaneOffset{FeatureRef{offset}, Param::of(-5)}});
  const Id p1 = tree.add(DatumPointFeature{PointAt{Param::of(0), Param::of(0), Param::of(3)}});
  const Id p2 = tree.add(DatumPointFeature{PointAt{Param::of(1), Param::of(0), Param::of(3)}});
  const Id p3 = tree.add(DatumPointFeature{PointAt{Param::of(0), Param::of(1), Param::of(3)}});
  const Id three =
      tree.add(DatumPlaneFeature{PlaneThroughPoints{FeatureRef{p1}, FeatureRef{p2}, FeatureRef{p3}}});
  // The XY plane turned 90° about the X axis is the plane y = 0 facing -Y.
  const Id tilted = tree.add(DatumPlaneFeature{
      PlaneAxisAngle{PrincipalAxis::X, PrincipalPlane::XY, Param::expr("90 deg")}});
  const Id tilted45 = tree.add(DatumPlaneFeature{
      PlaneAxisAngle{PrincipalAxis::X, PrincipalPlane::XY, Param::expr("45 deg")}});

  const auto r = rebuild(tree, kSettings);
  INFO(parts::codes(r.diagnostics));
  REQUIRE(r.ok());
  CHECK(r.body.is_empty());

  check_vec(ok_result(r, offset).plane->origin, {25, 0, 0});
  check_vec(ok_result(r, offset).plane->normal(), {1, 0, 0});
  check_vec(ok_result(r, offset2).plane->origin, {20, 0, 0});
  const auto& t = *ok_result(r, three).plane;
  check_vec(t.origin, {0, 0, 3});
  check_vec(t.normal(), {0, 0, 1});
  check_vec(ok_result(r, tilted).plane->normal(), {0, -1, 0});
  check_vec(ok_result(r, tilted45).plane->normal(), {0, -std::sqrt(0.5), std::sqrt(0.5)});
  check_vec(ok_result(r, tilted45).plane->x_axis, {1, 0, 0});
}

TEST_CASE("datum axes and points", "[feature][datum]") {
  FeatureTree tree;
  const Id a = tree.add(DatumPointFeature{PointAt{Param::of(1), Param::of(2), Param::of(3)}});
  const Id b = tree.add(DatumPointFeature{PointAt{Param::of(1), Param::of(2), Param::of(13)}});
  const Id through = tree.add(DatumAxisFeature{AxisThroughPoints{FeatureRef{a}, FeatureRef{b}}});
  const Id high = tree.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(7)}});
  const Id side = tree.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::YZ, Param::of(4)}});
  // The planes z = 7 and x = 4 meet along a line parallel to Y.
  const Id meet = tree.add(DatumAxisFeature{AxisPlaneIntersection{FeatureRef{high}, FeatureRef{side}}});
  const Id along = tree.add(DatumAxisFeature{AxisAlong{PrincipalAxis::Y}});
  // The axis through a and b meets z = 7 at (1, 2, 7).
  const Id hit = tree.add(DatumPointFeature{PointAxisPlane{FeatureRef{through}, FeatureRef{high}}});
  const Id copy = tree.add(DatumPointFeature{PointAtRef{FeatureRef{hit}}});

  const auto r = rebuild(tree, kSettings);
  INFO(parts::codes(r.diagnostics));
  REQUIRE(r.ok());
  check_vec(ok_result(r, through).axis->origin, {1, 2, 3});
  check_vec(ok_result(r, through).axis->direction, {0, 0, 1});
  const auto& m = *ok_result(r, meet).axis;
  check_vec(m.origin, {4, 0, 7});
  check_vec(m.direction, {0, 1, 0});  // cross(+Z, +X)
  check_vec(ok_result(r, along).axis->direction, {0, 1, 0});
  check_vec(*ok_result(r, hit).point, {1, 2, 7});
  check_vec(*ok_result(r, copy).point, {1, 2, 7});
}

TEST_CASE("degenerate datums are diagnosed", "[feature][datum]") {
  FeatureTree tree;
  const Id p = tree.add(DatumPointFeature{PointAt{Param::of(1), Param::of(1), Param::of(1)}});
  const Id q = tree.add(DatumPointFeature{PointAt{Param::of(2), Param::of(2), Param::of(2)}});
  const Id r3 = tree.add(DatumPointFeature{PointAt{Param::of(3), Param::of(3), Param::of(3)}});
  const Id same = tree.add(DatumAxisFeature{AxisThroughPoints{FeatureRef{p}, FeatureRef{p}}});
  const Id line =
      tree.add(DatumPlaneFeature{PlaneThroughPoints{FeatureRef{p}, FeatureRef{q}, FeatureRef{r3}}});
  const Id up = tree.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(3)}});
  const Id parallel =
      tree.add(DatumAxisFeature{AxisPlaneIntersection{PrincipalPlane::XY, FeatureRef{up}}});
  const Id perpendicular = tree.add(DatumPlaneFeature{
      PlaneAxisAngle{PrincipalAxis::Z, PrincipalPlane::XY, Param::of(1)}});
  const Id miss = tree.add(DatumPointFeature{PointAxisPlane{PrincipalAxis::X, PrincipalPlane::XY}});

  const auto r = rebuild(tree, kSettings);
  for (const Id& id : {same, line, parallel, perpendicular, miss}) {
    const auto* f = r.find(id);
    REQUIRE(f != nullptr);
    CHECK(f->status == FeatureStatus::Failed);
    REQUIRE(f->diagnostics.size() == 1);
    CHECK(f->diagnostics[0].code == "feature.datum.degenerate");
    CHECK(f->diagnostics[0].target()->id == id);
  }
}

TEST_CASE("sketches sit on datum planes, sketch planes and faces", "[feature][datum]") {
  FeatureTree tree;
  const Id raised = tree.add(DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(20)}});
  vs::sketch::Sketch square;
  const auto rect = parts::add_rectangle(square, {0, 0}, {10, 10});
  const Id on_datum = tree.add(SketchFeature{square, FeatureRef{raised}});
  ExtrudeFeature block;
  block.sketch = on_datum;
  block.depth = Param::of(5);
  const Id block_id = tree.add(block);
  // A second sketch on the first sketch's plane, and a third on the block's
  // right-hand side face.
  vs::sketch::Sketch again;
  parts::add_rectangle(again, {20, 0}, {25, 5});
  const Id on_sketch = tree.add(SketchFeature{again, FeatureRef{on_datum}});
  vs::sketch::Sketch boss;
  parts::add_rectangle(boss, {2, 22}, {8, 24});
  const Id on_face =
      tree.add(SketchFeature{boss, FaceRef{block_id, "side:" + rect.l[1].to_string()}});

  const auto r = rebuild(tree, kSettings);
  INFO(parts::codes(r.diagnostics));
  REQUIRE(r.ok());
  check_vec(ok_result(r, on_datum).plane->origin, {0, 0, 20});
  check_vec(ok_result(r, on_sketch).plane->origin, {0, 0, 20});
  // The right-hand side of the block is the plane x = 10 facing +X; its frame
  // has x along Y and y along Z.
  const auto& face_plane = *ok_result(r, on_face).plane;
  check_vec(face_plane.origin, {10, 0, 0});
  check_vec(face_plane.normal(), {1, 0, 0});
  check_vec(face_plane.x_axis, {0, 1, 0});
  const auto box = vs::kernel::bounding_box(r.body);
  check_vec(box.min, {0, 0, 20}, 1e-6);
  check_vec(box.max, {10, 10, 25}, 1e-6);
}

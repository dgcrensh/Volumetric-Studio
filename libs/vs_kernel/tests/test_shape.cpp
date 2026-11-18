#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/kernel.hpp"

using namespace vs::kernel;
using vs::core::dbox3;
using vs::core::ProductSettings;

namespace {
const ProductSettings kSettings = ProductSettings::defaults();

Solid unit_box() {
  return make_box(dbox3::from_min_max({0, 0, 0}, {1, 1, 1}), kSettings).value();
}
}  // namespace

TEST_CASE("a default shape is empty", "[kernel][shape]") {
  const Shape s;
  CHECK(s.is_empty());
  CHECK(s.kind() == ShapeKind::Empty);
  CHECK(Solid{}.is_empty());
  CHECK(s.is_same(Shape{}));
}

TEST_CASE("typed handles check their kind", "[kernel][shape]") {
  const Solid box = unit_box();
  CHECK(box.kind() == ShapeKind::Solid);
  CHECK(box.solid_count() == 1);

  const Shape& as_shape = box;
  CHECK(Solid::from(as_shape).ok());

  const auto not_face = Face::from(as_shape);
  REQUIRE_FALSE(not_face.ok());
  CHECK(not_face.error().code == "kernel.shape.wrong_kind");
  CHECK(Edge::from(as_shape).error().code == "kernel.shape.wrong_kind");
  CHECK(Shell::from(as_shape).error().code == "kernel.shape.wrong_kind");
  CHECK(Solid::from(Shape{}).error().code == "kernel.shape.wrong_kind");

  const auto box_faces = faces(box);
  REQUIRE(box_faces.size() == 6);
  CHECK(Face::from(box_faces[0]).ok());
  CHECK(Solid::from(box_faces[0]).error().code == "kernel.shape.wrong_kind");
}

TEST_CASE("handles share topology when copied", "[kernel][shape]") {
  const Solid a = unit_box();
  const Solid b = a;  // NOLINT(performance-unnecessary-copy-initialization)
  CHECK(a.is_same(b));
  CHECK_FALSE(a.is_same(unit_box()));
}

TEST_CASE("shape kinds have names", "[kernel][shape]") {
  CHECK(to_string(ShapeKind::Solid) == "solid");
  CHECK(to_string(ShapeKind::Empty) == "empty");
  CHECK(to_string(ShapeKind::Edge) == "edge");
}

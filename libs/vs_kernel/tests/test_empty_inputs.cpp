// Plan H1: the kernel never crashes or throws on an empty handle or a point
// that isn't finite; it answers "nothing" (NaN, nullopt, outside).

#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "vs/kernel/kernel.hpp"

using namespace vs::kernel;
using vs::core::dbox3;
using vs::core::dvec2;
using vs::core::dvec3;

namespace {
const vs::core::ProductSettings kSettings = vs::core::ProductSettings::defaults();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool is_nan(const dvec3& p) { return std::isnan(p.x) && std::isnan(p.y) && std::isnan(p.z); }
bool is_nan(const dvec2& p) { return std::isnan(p.x) && std::isnan(p.y); }

Solid unit_box() { return make_box(dbox3::from_min_max({0, 0, 0}, {10, 10, 10}), kSettings).value(); }
}  // namespace

TEST_CASE("Edge and face points of empty handles are NaN", "[kernel][h1]") {
  CHECK(is_nan(edge_point(Edge{}, 0.0)));
  CHECK(is_nan(edge_uv(Face{}, Edge{}, 0.0)));
  CHECK(is_nan(face_point(Face{}, {0.0, 0.0})));
  const Solid box = unit_box();
  const Face f = faces(box).front();
  const Edge e = edges(f).front();
  CHECK(is_nan(edge_uv(f, Edge{}, 0.0)));
  CHECK(is_nan(edge_uv(Face{}, e, 0.0)));
}

TEST_CASE("Edge parameters of empty edges and NaN points are none", "[kernel][h1]") {
  CHECK_FALSE(edge_parameter(Edge{}, {0, 0, 0}, 1e-6).has_value());
  const Edge e = edges(unit_box()).front();
  CHECK_FALSE(edge_parameter(e, {kNaN, 0, 0}, 1e-6).has_value());
  CHECK_FALSE(edge_parameter(e, edge_point(e, 0.0), kNaN).has_value());
}

TEST_CASE("Classifying NaN points or empty solids says outside", "[kernel][h1]") {
  const Solid box = unit_box();
  CHECK(classify(box, {kNaN, 5, 5}, kSettings) == PointClass::Outside);
  CHECK(classify(Solid{}, {5, 5, 5}, kSettings) == PointClass::Outside);
  const SolidClassifier classifier(box, kSettings);
  CHECK(classifier.classify({5, kNaN, 5}) == PointClass::Outside);
  CHECK(classifier.classify({5, 5, 5}) == PointClass::Inside);
  CHECK(SolidClassifier(Solid{}, kSettings).classify({5, 5, 5}) == PointClass::Outside);
}

TEST_CASE("Queries of empty handles answer nothing", "[kernel][h1]") {
  CHECK(length(Edge{}) == 0.0);
  CHECK_FALSE(is_seam(Edge{}, Face{}));
  CHECK_FALSE(face_plane(Face{}).has_value());
  CHECK_FALSE(face_cylinder(Face{}).has_value());
  CHECK(face_loops(Face{}).empty());
  CHECK(surface_area(Shape{}) == 0.0);
  CHECK(audit_tolerances(Shape{}, kSettings).edges.count == 0);
  CHECK_FALSE(mass_properties(Solid{}).ok());
  CHECK(vertex_count(Shape{}) == 0);
}

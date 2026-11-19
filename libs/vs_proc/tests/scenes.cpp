#include "scenes.hpp"

#include <algorithm>
#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "vs/feature/tree.hpp"
#include "vs/model/check.hpp"

namespace scenes {

using namespace vs::model;
using vs::core::dvec3;
using vs::feature::DatumPlaneFeature;
using vs::feature::DatumPointFeature;
using vs::feature::FeatureRef;
using vs::feature::Param;
using vs::feature::PlaneOffset;
using vs::feature::PointAt;
using vs::feature::PrincipalPlane;
using vs::proc::Graph;
using vs::proc::Setting;

namespace {

Id point(Document& doc, const std::string& name, dvec3 p) {
  return doc.layout.add(DatumPointFeature{PointAt{Param::of(p.x), Param::of(p.y), Param::of(p.z)}},
                        name);
}

}  // namespace

Boxes boxes(const std::vector<Graph>& graphs) {
  Boxes b;
  Document& doc = b.project.document;
  doc = make_document("Boxes");
  for (std::size_t i = 0; i < graphs.size(); ++i) {
    const double x0 = 40.0 * static_cast<double>(i);
    const std::string name = fmt::format("P{}", i);
    Volume v;
    v.id = Id::random();
    v.name = name;
    v.boundary = BoxBoundary{FeatureRef{point(doc, name + " min", {x0, 0, 0})},
                             FeatureRef{point(doc, name + " max", {x0 + 40, 30, 20})}};
    const Id wall = Id::random();
    v.parameters.push_back({wall, "wall", Param::of(2)});
    v.outputs.push_back({Id::random(), "volume", OutputQuantity::Volume, 1.0});
    v.seed = i;
    v.contents = ProceduralContents{vs::proc::to_json(graphs[i])};
    b.volumes.push_back(v.id);
    b.wall.push_back(wall);
    doc.volumes.push_back(std::move(v));
  }
  for (std::size_t i = 0; i + 1 < graphs.size(); ++i) {
    const Id plane = doc.layout.add(
        DatumPlaneFeature{PlaneOffset{PrincipalPlane::YZ, Param::of(40.0 * static_cast<double>(i + 1))}},
        fmt::format("Joint {}", i));
    ConnectionSurface s;
    s.id = Id::random();
    s.name = fmt::format("S{}", i);
    DatumPatch dp;
    dp.plane = FeatureRef{plane};
    dp.u = Param::of(10);
    dp.v = Param::of(10);
    dp.width = Param::of(10);
    dp.height = Param::of(10);
    s.geometry = dp;
    s.volumes = {b.volumes[i], b.volumes[i + 1]};
    s.params = BondedParams{};
    b.surfaces.push_back(s.id);
    doc.connections.push_back(std::move(s));
  }
  INFO(codes(check(doc)));
  REQUIRE_FALSE(check(doc).has_errors());
  return b;
}

ShellAndBall shell_and_ball() {
  ShellAndBall s;
  Graph& g = s.graph;
  g.parameters.push_back({"wall", 2.0});
  s.boundary = g.add("input.boundary", "Boundary");
  s.wall = g.add("input.parameter", "Wall", {{"name", Setting(std::string("wall"))}});
  s.shell = g.add("field.shell", "Shell");
  s.ball = g.add("field.sphere", "Ball",
                 {{"centre", Setting(dvec3{20, 15, 10})}, {"radius", Setting(6.0)}});
  s.join = g.add("field.union", "Join");
  s.output = g.add("output", "Output", {{"cell_size", Setting(1.0)}});
  g.link(s.boundary, "field", s.shell, "field");
  g.link(s.wall, "value", s.shell, "thickness");
  g.link(s.shell, "field", s.join, "a");
  g.link(s.ball, "field", s.join, "b");
  g.link(s.join, "field", s.output, "contents");
  return s;
}

double box_distance(const dvec3& p, const dvec3& lo, const dvec3& hi) {
  const dvec3 c = (lo + hi) * 0.5;
  const dvec3 h = (hi - lo) * 0.5;
  const dvec3 q{std::abs(p.x - c.x) - h.x, std::abs(p.y - c.y) - h.y, std::abs(p.z - c.z) - h.z};
  const dvec3 o{std::max(q.x, 0.0), std::max(q.y, 0.0), std::max(q.z, 0.0)};
  return std::sqrt(o.x * o.x + o.y * o.y + o.z * o.z) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0);
}

double shell_and_ball_value(const dvec3& p, const dvec3& lo, const dvec3& hi, double wall) {
  const double d = box_distance(p, lo, hi);
  const double shell = std::max(d, -d - wall);
  const dvec3 c{20, 15, 10};  // the graph places the ball absolutely
  const dvec3 r = p - c;
  const double ball = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z) - 6.0;
  return std::min(shell, ball);
}

Id constant_output(Graph& graph, const Id& source, const std::string& value_port) {
  const Id c = graph.add("field.constant", "Constant");
  const Id out = graph.add("output", "Output");
  graph.link(source, value_port, c, "value");
  graph.link(c, "field", out, "contents");
  return c;
}

vs::proc::GraphInputs bare_inputs(std::uint64_t seed) {
  vs::proc::GraphInputs in;
  in.volume = Id::random();
  in.volume_subject = {vs::core::SubjectKind::Volume, in.volume, "Test volume"};
  in.seed = seed;
  in.settings = vs::core::ProductSettings::defaults();
  return in;
}

std::string codes(const vs::core::Diagnostics& diagnostics) {
  std::string out;
  for (const auto& d : diagnostics) out += d.to_string() + "\n";
  return out;
}

}  // namespace scenes

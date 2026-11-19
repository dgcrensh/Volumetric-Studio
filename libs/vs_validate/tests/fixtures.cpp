#include "fixtures.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

#include "vs/core/settings.hpp"
#include "vs/kernel/exchange.hpp"
#include "vs/kernel/primitives.hpp"

namespace fixtures {

using namespace vs::model;
using vs::feature::DatumPlaneFeature;
using vs::feature::DatumPointFeature;
using vs::feature::FeatureRef;
using vs::feature::Param;
using vs::feature::PlaneOffset;
using vs::feature::PointAt;
using vs::feature::PrincipalPlane;

namespace {

Id point(Document& doc, const std::string& name, dvec3 p) {
  return doc.layout.add(
      DatumPointFeature{PointAt{Param::of(p.x), Param::of(p.y), Param::of(p.z)}}, name);
}

}  // namespace

Id add_box(Document& doc, const std::string& name, dvec3 lo, dvec3 hi) {
  const Id a = point(doc, name + " min", lo);
  const Id b = point(doc, name + " max", hi);
  Volume v;
  v.id = Id::random();
  v.name = name;
  v.boundary = BoxBoundary{FeatureRef{a}, FeatureRef{b}};
  const Id id = v.id;
  doc.volumes.push_back(std::move(v));
  return id;
}

Id add_patch(Document& doc, const std::string& name, std::vector<Id> volumes, double z, double u,
             double v, double width, double height, ConnectionParams params) {
  const Id plane = doc.layout.add(
      DatumPlaneFeature{PlaneOffset{PrincipalPlane::XY, Param::of(z)}}, name + " plane");
  ConnectionSurface c;
  c.id = Id::random();
  c.name = name;
  DatumPatch patch;
  patch.plane = FeatureRef{plane};
  patch.u = Param::of(u);
  patch.v = Param::of(v);
  patch.width = Param::of(width);
  patch.height = Param::of(height);
  c.geometry = patch;
  c.volumes = std::move(volumes);
  c.params = std::move(params);
  const Id id = c.id;
  doc.connections.push_back(std::move(c));
  return id;
}

BoltedFlangeParams bolts(const std::string& thread, double min_wall, int columns, double pitch,
                         double u, double v) {
  BoltedFlangeParams b;
  BoltGrid grid;
  grid.columns = Param::of(columns);
  grid.rows = Param::of(1);
  grid.pitch_x = Param::of(pitch);
  b.holes.layout = grid;
  b.holes.u = Param::of(u);
  b.holes.v = Param::of(v);
  b.thread = thread;
  b.min_wall = Param::of(min_wall);
  return b;
}

Blob step_box(dvec3 lo, dvec3 hi) {
  const auto settings = vs::core::ProductSettings::defaults();
  auto box = vs::kernel::make_box(vs::core::dbox3::from_min_max(lo, hi), settings);
  if (!box) throw std::runtime_error(box.error().message);
  const auto path = std::filesystem::temp_directory_path() /
                    ("vs_validate_box_" + Id::random().to_string() + ".step");
  if (vs::kernel::write_step(*box, path).has_errors()) throw std::runtime_error("write_step");
  std::ifstream in(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  std::filesystem::remove(path);
  Blob out(text.size());
  std::transform(text.begin(), text.end(), out.begin(),
                 [](char c) { return static_cast<std::byte>(c); });
  return out;
}

namespace {

Fixture clean() {
  Fixture f{"clean", {}, {}};
  Document& doc = f.project.document = make_document("Clean");
  const Id base = add_box(doc, "Base", {0, 0, 0}, {60, 40, 10});
  const Id top = add_box(doc, "Top", {0, 0, 10}, {60, 40, 30});
  add_patch(doc, "Flange", {base, top}, 10, 30, 20, 60, 40, bolts("M4", 2, 2, 40, 30, 20));
  const std::string blob = f.project.add_blob(step_box({0, 0, 10}, {60, 40, 30}), "step");
  doc.find_volume(top)->contents = StaticContents{blob, "step", {}};
  return f;
}

Fixture overlap() {
  Fixture f{"overlap", {}, {"validate.overlap"}};
  Document& doc = f.project.document = make_document("Overlap");
  add_box(doc, "Left", {0, 0, 0}, {40, 40, 20});
  add_box(doc, "Right", {30, 0, 0}, {70, 40, 20});
  return f;
}

Fixture contact() {
  Fixture f{"contact", {}, {"validate.contact", "validate.contact.edge"}};
  Document& doc = f.project.document = make_document("Contact");
  add_box(doc, "Left", {0, 0, 0}, {20, 20, 20});
  add_box(doc, "Middle", {20, 0, 0}, {40, 20, 20});
  add_box(doc, "Corner", {40, 20, 0}, {60, 40, 20});
  return f;
}

Fixture mismatch() {
  Fixture f{"mismatch", {}, {"validate.connection.edge_wall", "validate.connection.hole_spacing"}};
  Document& doc = f.project.document = make_document("Mismatch");
  const Id base = add_box(doc, "Base", {0, 0, 0}, {20, 10, 10});
  const Id top = add_box(doc, "Top", {0, 0, 10}, {20, 10, 20});
  // M6 clearance holes are 6.6 mm. With a 3 mm wall each centre must be
  // 6.3 mm inside the 20 x 10 patch's edge (it is 5 mm), and the two
  // centres 9.6 mm apart (they are 8 mm).
  add_patch(doc, "Flange", {base, top}, 10, 10, 5, 20, 10, bolts("M6", 3, 2, 8, 10, 5));
  return f;
}

Fixture loop() {
  Fixture f{"loop", {}, {"model.dependency.loop"}};
  Document& doc = f.project.document = make_document("Loop");
  const Id h = doc.layout.variables().add("h", "10 mm").value();
  const Id lo = point(doc, "Block min", {0, 0, 0});
  const Id hi = doc.layout.add(
      DatumPointFeature{PointAt{Param::of(20), Param::of(20), Param::expr("h")}}, "Block max");
  Volume v;
  v.id = Id::random();
  v.name = "Block";
  v.boundary = BoxBoundary{FeatureRef{lo}, FeatureRef{hi}};
  const Id out = Id::random();
  v.outputs.push_back({out, "volume", OutputQuantity::Volume, 1.0});
  doc.links.push_back({Id::random(), {v.id, out}, h});
  doc.volumes.push_back(std::move(v));
  return f;
}

Fixture containment() {
  Fixture f{"containment", {}, {"validate.containment"}};
  Document& doc = f.project.document = make_document("Containment");
  const Id block = add_box(doc, "Block", {0, 0, 0}, {20, 20, 20});
  const std::string blob = f.project.add_blob(step_box({0, 0, 0}, {25, 20, 20}), "step");
  doc.find_volume(block)->contents = StaticContents{blob, "step", {}};
  return f;
}

}  // namespace

std::vector<Fixture> all() {
  return {clean(), overlap(), contact(), mismatch(), loop(), containment()};
}

Fixture by_name(const std::string& name) {
  for (auto& f : all()) {
    if (f.name == name) return f;
  }
  throw std::runtime_error("no fixture " + name);
}

std::string codes(const vs::core::Diagnostics& diagnostics) {
  std::vector<std::string> list;
  for (const auto& d : diagnostics) list.push_back(d.code);
  return joined(list);
}

std::string joined(const std::vector<std::string>& codes) {
  std::ostringstream s;
  for (std::size_t i = 0; i < codes.size(); ++i) s << (i ? " " : "") << codes[i];
  return s.str();
}

}  // namespace fixtures

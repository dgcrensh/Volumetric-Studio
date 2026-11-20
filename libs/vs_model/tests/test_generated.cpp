// L4: generated layouts in the model: templates and generators in files,
// expand(), and the checks around them.

#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "nested.hpp"
#include "rack.hpp"
#include "vs/feature/serialize.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"
#include "vs/model/generated.hpp"
#include "vs/model/hierarchy.hpp"
#include "vs/model/project.hpp"

using namespace vs::model;
using Catch::Matchers::WithinAbs;
using vs::core::dbox3;
using vs::core::dvec3;
using vs::core::Id;
using vs::feature::Param;

namespace {

const auto kSettings = ProductSettings::defaults();

std::string messages(const Diagnostics& ds) {
  std::string out;
  for (const auto& d : ds) out += d.code + ": " + d.message + "\n";
  return out;
}

bool has_code(const Diagnostics& ds, const std::string& code) {
  for (const auto& d : ds) {
    if (d.code == code) return true;
  }
  return false;
}

// Two cells side by side along x, bonded at x = 20; the first meets Seat.
GeneratedLayout two_cells(const Id& seat) {
  GeneratedLayout g;
  GeneratedChild a;
  a.key = Id::derive(Id{}, "a");
  a.name = "Cell 1";
  a.template_name = "Cell";
  a.box = dbox3::from_min_max({0, 0, 0}, {20, 20, 10});
  a.seed = 7;
  GeneratedChild b = a;
  b.key = Id::derive(Id{}, "b");
  b.name = "Cell 2";
  b.box = dbox3::from_min_max({20, 0, 0}, {40, 20, 10});
  b.parameters.emplace("depth", 4.0);
  g.children = {a, b};
  GeneratedSurface s;
  s.key = Id::derive(Id{}, "ab");
  s.name = "Cell 1 / Cell 2";
  s.a = 0;
  s.b = 1;
  s.axis = 0;
  s.offset = 20;
  s.lo = {1, 1};
  s.hi = {19, 9};
  g.surfaces = {s};
  g.delegations = {{seat, 0}};
  return g;
}

const Volume& child(const Document& doc, const Id& rack, const Id& key) {
  const Volume* v = doc.find_volume(generated_child_id(rack, key));
  REQUIRE(v != nullptr);
  return *v;
}

}  // namespace

TEST_CASE("A generating composite and its templates round-trip through JSON", "[generated]") {
  rack::Rack r = rack::make_rack(R"({"format": "vstudio.graph"})");
  auto& comp = std::get<CompositeContents>(r.project.document.find_volume(r.rack)->contents);
  comp.templates.front().origin = {1, 2, 3};
  ChildTemplate blank;
  blank.id = Id::random();
  blank.name = "Blank";
  comp.templates.push_back(blank);
  const Document& doc = r.project.document;
  REQUIRE_FALSE(check(doc).has_errors());

  const std::string text = to_json(doc);
  auto back = document_from_json(text);
  REQUIRE(back);
  CHECK(*back == doc);
  CHECK(to_json(*back) == text);
  CHECK(text.find("\"generator\"") != std::string::npos);
  CHECK(text.find("\"templates\"") != std::string::npos);

  // An L3 document writes none of it, so its JSON and hash are unchanged.
  const nested::Nested n = nested::make_nested();
  const std::string flat = to_json(n.project.document);
  CHECK(flat.find("\"generator\"") == std::string::npos);
  CHECK(flat.find("\"templates\"") == std::string::npos);
  CHECK(flat.find("\"generated\"") == std::string::npos);
  CHECK(flat.find("\"contents_offset\"") == std::string::npos);
}

TEST_CASE("expand() writes a generated layout in as if authored", "[generated]") {
  const rack::Rack r = rack::make_rack();
  const Document& doc = r.project.document;
  const GeneratedLayout g = two_cells(r.seat);

  auto expanded = expand(doc, r.rack, g);
  REQUIRE(expanded);
  const Document& e = *expanded;
  INFO(messages(check(e)));
  REQUIRE_FALSE(check(e).has_errors());

  // Authored volumes keep their places; children come after them.
  REQUIRE(e.volumes.size() == 4);
  CHECK(e.volumes[0].id == r.base);
  CHECK(e.volumes[1].id == r.rack);
  const Volume& a = child(e, r.rack, g.children[0].key);
  const Volume& b = child(e, r.rack, g.children[1].key);
  CHECK(a.generated);
  CHECK(a.parent == r.rack);
  CHECK(a.seed == 7);
  CHECK(a.contents_offset == dvec3{0, 0, 0});
  CHECK(b.contents_offset == dvec3{20, 0, 0});

  // The template's ids are the child's now; its parameter took the value.
  const std::string tree = vs::feature::to_json(std::get<ParametricContents>(b.contents).tree);
  CHECK(tree.find(b.id.to_string()) != std::string::npos);
  CHECK(tree.find(r.cell.to_string()) == std::string::npos);
  REQUIRE(b.parameters.size() == 1);
  CHECK(b.parameters[0].value.value == 4.0);
  CHECK_FALSE(b.parameters[0].id == r.depth);
  CHECK(a.parameters[0].value.value == 10.0);

  // The surface joins them, and Seat is passed to the first.
  REQUIRE(e.connections.size() == 2);
  const ConnectionSurface* s = e.find_connection(generated_surface_id(r.rack, g.surfaces[0].key));
  REQUIRE(s != nullptr);
  CHECK(s->volumes == std::vector<Id>{a.id, b.id});
  CHECK(delegate_of(e, r.rack, r.seat) == a.id);
  CHECK(seam_sides(e, *e.find_connection(r.seat)) == std::vector<Id>{r.base, a.id});
  CHECK(generator_of(e, b.id) == r.rack);
  CHECK(generator_of(e, r.base).is_null());

  // The same layout gives the same document.
  auto again = expand(doc, r.rack, g);
  REQUIRE(again);
  CHECK(*again == e);
  CHECK(to_json(*again) == to_json(e));
}

TEST_CASE("Expanded children build where their boxes are", "[generated]") {
  const rack::Rack r = rack::make_rack();
  const GeneratedLayout g = two_cells(r.seat);
  auto e = expand(r.project.document, r.rack, g);
  REQUIRE(e);
  const DocumentBuild built = build(*e, kSettings);
  INFO(messages(built.diagnostics));
  for (const Id& key : {g.children[0].key, g.children[1].key}) {
    const Volume& v = child(*e, r.rack, key);
    const std::size_t i = static_cast<std::size_t>(v.id == e->volumes[2].id ? 2 : 3);
    REQUIRE(e->volumes[i].id == v.id);
    const VolumeBuild& vb = built.volumes[i];
    CHECK(vb.status == VolumeStatus::Current);
    REQUIRE_FALSE(vb.contents.is_empty());
    const dbox3 box = vs::kernel::bounding_box(vb.contents);
    const double x0 = key == g.children[0].key ? 0.0 : 20.0;
    const double depth = key == g.children[0].key ? 10.0 : 4.0;
    CHECK_THAT(box.min.x, WithinAbs(x0, 1e-6));
    CHECK_THAT(box.max.x, WithinAbs(x0 + 20, 1e-6));
    CHECK_THAT(box.min.z, WithinAbs(10 - depth, 1e-6));
    CHECK_THAT(box.max.z, WithinAbs(10, 1e-6));
  }
}

TEST_CASE("expand() refuses layouts it can't write", "[generated]") {
  const rack::Rack r = rack::make_rack();
  const Document& doc = r.project.document;

  auto code_of = [&](const GeneratedLayout& g, const Id& volume) {
    auto e = expand(doc, volume, g);
    return e ? std::string() : e.error().code;
  };
  GeneratedLayout g = two_cells(r.seat);
  CHECK(code_of(g, r.base) == "model.generated.not_generator");

  GeneratedLayout t = g;
  t.children[1].template_name = "Nope";
  CHECK(code_of(t, r.rack) == "model.generated.template");

  GeneratedLayout p = g;
  p.children[1].parameters.emplace("width", 3.0);
  CHECK(code_of(p, r.rack) == "model.generated.parameter");

  GeneratedLayout s = g;
  s.surfaces[0].b = 5;
  CHECK(code_of(s, r.rack) == "model.generated.surface");

  GeneratedLayout flat = g;
  flat.surfaces[0].hi = flat.surfaces[0].lo;
  CHECK(code_of(flat, r.rack) == "model.generated.surface");

  GeneratedLayout empty = g;
  empty.children[0].box = dbox3::empty();
  CHECK(code_of(empty, r.rack) == "model.generated.child");
}

TEST_CASE("Generated names needn't be distinct, and the expansion is checked", "[generated]") {
  const rack::Rack r = rack::make_rack();
  GeneratedLayout g = two_cells(r.seat);
  g.children[1].name = g.children[0].name;
  g.children[0].name = "Base";  // an authored volume's name
  auto e = expand(r.project.document, r.rack, g);
  REQUIRE(e);
  CHECK_FALSE(check(*e).has_errors());

  // The same key twice is two volumes with one id.
  GeneratedLayout twice = two_cells(r.seat);
  twice.children[1].key = twice.children[0].key;
  auto bad = expand(r.project.document, r.rack, twice);
  REQUIRE(bad);
  CHECK(has_code(check(*bad), "model.document.duplicate_id"));
}

TEST_CASE("Templates and links into a generator are checked", "[generated]") {
  rack::Rack r = rack::make_rack();
  Document& doc = r.project.document;
  auto& comp = std::get<CompositeContents>(doc.find_volume(r.rack)->contents);

  SECTION("templates") {
    comp.templates.push_back(comp.templates.front());
    comp.templates.back().id = Id::random();
    CHECK(has_code(check(doc), "model.composite.template"));
    comp.templates.back().name = "Other";
    comp.templates.back().parameters.front().id = Id::random();
    comp.templates.back().outputs.front().id = Id::random();
    // Its tree's ids are the first template's: one id, two things.
    CHECK(has_code(check(doc), "model.document.duplicate_id"));
    comp.templates.back().contents = rack::cell_tree(comp.templates.back().id, {20, 20, 10});
    CHECK_FALSE(check(doc).has_errors());
    comp.templates.back().parameters.front().name = "2bad";
    CHECK(has_code(check(doc), "model.composite.template"));
  }

  SECTION("links from outside into its inputs") {
    Volume* rack_volume = doc.find_volume(r.rack);
    const Id count = Id::random();
    rack_volume->parameters.push_back({count, "count", Param::of(3)});
    Volume* base = doc.find_volume(r.base);
    const Id out = Id::random();
    base->outputs.push_back({out, "volume", OutputQuantity::Volume, 1.0});
    doc.links.push_back({Id::random(), {r.base, out}, count});
    CHECK(has_code(check(doc), "model.composite.generated_link"));
    // Without a generator, it's an ordinary link.
    std::get<CompositeContents>(rack_volume->contents).generator.clear();
    CHECK_FALSE(has_code(check(doc), "model.composite.generated_link"));
  }
}

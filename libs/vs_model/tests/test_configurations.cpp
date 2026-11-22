// L5: configurations and design tables in the document (plan
// docs/plans/L5-configurations-and-design-tables.md, tests 1 to 3).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "bracket.hpp"
#include "nested.hpp"
#include "rack.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/model/build.hpp"
#include "vs/model/check.hpp"
#include "vs/model/configuration.hpp"
#include "vs/model/design_table.hpp"
#include "vs/model/project.hpp"

using namespace vs::model;
using bracket::codes;
using vs::core::Id;
using Catch::Matchers::WithinAbs;

namespace {

bool has(const vs::core::Diagnostics& ds, std::string_view code) {
  return std::any_of(ds.begin(), ds.end(), [&](const auto& d) { return d.code == code; });
}

Configuration config(std::string name, std::vector<ConfigurationValue> values) {
  return {Id::random(), std::move(name), std::move(values)};
}

const VolumeParameter& parameter(const Document& d, const Id& id) {
  const VolumeParameter* found = nullptr;
  for (const auto& v : d.volumes) {
    if (const auto* p = v.find_parameter(id)) found = p;
  }
  REQUIRE(found != nullptr);
  return *found;
}

// Nested with a variable in Housing's layout (Housing/inset).
struct NestedWithVariable {
  nested::Nested n;
  Id inset;
};
NestedWithVariable nested_with_variable() {
  NestedWithVariable out{nested::make_nested(), {}};
  auto* housing = out.n.project.document.find_volume(out.n.housing);
  out.inset = *housing->tree()->variables().add("inset", "1");
  return out;
}

}  // namespace

TEST_CASE("Configurations round-trip and leave other documents unchanged",
          "[model][configuration]") {
  auto b = bracket::make_bracket();
  Document& doc = b.project.document;
  const auto before_json = to_json(doc);
  const auto before_hash = content_hash(b.project);
  CHECK(before_json.find("configurations") == std::string::npos);
  CHECK(before_json.find("active_configuration") == std::string::npos);
  CHECK(configured(doc) == doc);

  doc.configurations.push_back(config("Wide", {{b.width, "80 mm"}, {b.base_thickness, "6"}}));
  doc.configurations.push_back(config("Thin", {{b.base_thickness, "base_h / 2"}}));
  doc.active_configuration = doc.configurations[1].id;
  CHECK_FALSE(check(doc).has_errors());

  const auto text = to_json(doc);
  const auto back = document_from_json(text);
  REQUIRE(back);
  CHECK(*back == doc);
  CHECK(to_json(*back) == text);
  CHECK(content_hash(b.project) != before_hash);

  // Switching the active configuration is a change of the document.
  Document other = doc;
  other.active_configuration = doc.configurations[0].id;
  CHECK(content_hash(other) != content_hash(doc));

  // Removing them gives the original back, byte for byte.
  doc.configurations.clear();
  doc.active_configuration = Id::null();
  CHECK(to_json(doc) == before_json);
  CHECK(content_hash(b.project) == before_hash);
}

TEST_CASE("Applying a configuration writes each kind of target", "[model][configuration]") {
  auto [n, inset] = nested_with_variable();
  Document& doc = n.project.document;
  doc.configurations.push_back(
      config("Big", {{n.w, "60"}, {n.width, "w - 5"}, {inset, "2.5"}}));
  doc.configurations.push_back(config("Plain", {{n.width, "40"}}));
  REQUIRE_FALSE(check(doc).has_errors());

  const Document big = apply_configuration(doc, doc.configurations[0].id);
  CHECK(big.configurations.empty());
  CHECK(big.active_configuration.is_null());
  CHECK(big.layout.variables().find(n.w)->expression == "60");
  CHECK(parameter(big, n.width).value == vs::feature::Param::expr("w - 5"));
  CHECK(big.find_volume(n.housing)->tree()->variables().find(inset)->expression == "2.5");
  // Everything else is as authored.
  CHECK(big.volumes.size() == doc.volumes.size());
  CHECK(big.connections == doc.connections);

  // A plain number becomes the parameter's value, not an expression.
  const Document plain = apply_configuration(doc, doc.configurations[1].id);
  CHECK(parameter(plain, n.width).value == vs::feature::Param::of(40));

  // configured(): the active one, or the authored values.
  CHECK(configured(doc) == apply_configuration(doc, Id::null()));
  doc.active_configuration = doc.configurations[0].id;
  CHECK(configured(doc) == big);
  CHECK_FALSE(check(big).has_errors());

  // The labels and drivable targets.
  const auto all = drivable_targets(doc);
  std::vector<std::string> labels;
  for (const auto& d : all) labels.push_back(d.label);
  CHECK(labels == std::vector<std::string>{"w", "Housing.width", "Housing/inset"});
  CHECK(drivable(doc, n.width)->kind == DrivableKind::Parameter);
  CHECK(drivable(doc, inset)->kind == DrivableKind::CompositeVariable);
  CHECK(drivable(doc, inset)->owner == n.housing);
  CHECK_FALSE(drivable(doc, n.housing));
  CHECK_FALSE(drivable(doc, n.seat));
}

TEST_CASE("The bracket builds its active configuration", "[model][configuration]") {
  auto b = bracket::make_bracket();
  Document& doc = b.project.document;
  const auto settings = vs::core::ProductSettings::defaults();
  const auto base_volume = [&](const DocumentBuild& built) {
    for (const auto& o : built.volumes[0].outputs) {
      if (o.output == b.base_volume && o.value) return std::get<double>(*o.value);
    }
    return -1.0;
  };
  const auto authored = build(doc, settings, &b.project.blobs);
  CHECK_THAT(base_volume(authored), WithinAbs(60.0 * 40 * 10, 1e-6));

  doc.configurations.push_back(config("Thin", {{b.base_thickness, "base_h / 2"}}));
  doc.configurations.push_back(config("Wide", {{b.width, "80"}}));
  doc.active_configuration = doc.configurations[0].id;
  REQUIRE_FALSE(check(doc).has_errors());
  CHECK_THAT(base_volume(build(doc, settings, &b.project.blobs)), WithinAbs(60.0 * 40 * 5, 1e-6));

  // The layout follows a variable; the Base's contents are a fixed
  // 60 x 40 rectangle, so only its boundary grows.
  doc.active_configuration = doc.configurations[1].id;
  const auto wide = build(doc, settings, &b.project.blobs);
  CHECK(vs::kernel::bounding_box(wide.volumes[0].boundary).max.x > 79.0);
}

TEST_CASE("check() refuses configurations that wouldn't build", "[model][configuration]") {
  auto b = bracket::make_bracket();
  Document& doc = b.project.document;
  const auto refused = [&](Configuration c, std::string_view code) {
    Document d = doc;
    d.configurations.push_back(std::move(c));
    const auto ds = check(d);
    INFO(codes(ds));
    CHECK(has(ds, code));
    CHECK(ds.has_errors());
    // The configuration is the subject to act on.
    for (const auto& diag : ds) {
      if (diag.code == code) {
        CHECK(diag.subjects.front().kind == vs::core::SubjectKind::Configuration);
      }
    }
  };

  SECTION("names") {
    refused(config("", {}), "model.configuration.name");
    Document d = doc;
    d.configurations.push_back(config("A", {}));
    d.configurations.push_back(config("A", {}));
    CHECK(has(check(d), "model.configuration.name"));
  }
  SECTION("targets") {
    refused(config("A", {{b.base, "1"}}), "model.configuration.target");          // a volume
    refused(config("A", {{b.inner_extrude, "1"}}), "model.configuration.target"); // contents
    refused(config("A", {{Id::random(), "1"}}), "model.configuration.target");    // nothing
    refused(config("A", {{b.width, "70"}, {b.width, "80"}}), "model.configuration.target");
  }
  SECTION("expressions") {
    refused(config("A", {{b.width, "70 +"}}), "model.configuration.expression");
    refused(config("A", {{b.width, " "}}), "model.configuration.expression");
    refused(config("A", {{b.width, "nope * 2"}}), "model.configuration.expression");
    // A variable loop: width uses depth, depth uses width.
    refused(config("A", {{b.width, "depth"}, {b.depth, "width"}}), "model.configuration.expression");
    // A parameter naming something it can't see: a variable inside the
    // Base's contents is no layout variable.
    nested::Nested n = nested::make_nested();
    Document nd = n.project.document;
    auto* housing = nd.find_volume(n.housing);
    const Id inner = *housing->tree()->variables().add("inner", "3");
    (void)inner;
    nd.configurations.push_back(config("A", {{n.width, "inner"}}));
    CHECK(has(check(nd), "model.configuration.expression"));
  }
  SECTION("loops through links") {
    // Base's mass drives upright_h; a configuration making the Base's
    // thickness use upright_h closes Base → mass → upright_h → Base.
    Document d = doc;
    d.links.push_back({Id::random(), {b.base, b.base_mass}, b.upright_h});
    REQUIRE_FALSE(check(d).has_errors());
    d.configurations.push_back(config("A", {{b.base_thickness, "upright_h / 4"}}));
    const auto ds = check(d);
    INFO(codes(ds));
    CHECK(has(ds, "model.configuration.loop"));
  }
  SECTION("the active configuration") {
    Document d = doc;
    d.active_configuration = Id::random();
    CHECK(has(check(d), "model.configuration.active"));
  }
  SECTION("a linked target is a warning") {
    Document d = doc;
    d.configurations.push_back(config("A", {{b.upright_load, "5"}}));
    const auto ds = check(d);
    CHECK(has(ds, "model.configuration.linked"));
    CHECK_FALSE(ds.has_errors());
  }
  SECTION("ids are the document's") {
    Document d = doc;
    d.configurations.push_back({b.width, "A", {}});
    CHECK(has(check(d), "model.document.duplicate_id"));
  }
}

TEST_CASE("Configurations drive a generating composite's parameters", "[model][configuration]") {
  auto r = rack::make_rack();
  Document& doc = r.project.document;
  auto* rack_volume = doc.find_volume(r.rack);
  const Id columns = Id::random();
  rack_volume->parameters.push_back({columns, "columns", vs::feature::Param::of(3)});
  doc.configurations.push_back(config("Two", {{columns, "2"}}));
  doc.configurations.push_back(config("Four", {{columns, "4"}}));
  doc.active_configuration = doc.configurations[1].id;
  REQUIRE_FALSE(check(doc).has_errors());
  CHECK(parameter(configured(doc), columns).value == vs::feature::Param::of(4));
  // Template parameters aren't drivable: the generator sets them.
  CHECK_FALSE(drivable(doc, r.depth));
}

TEST_CASE("Pruning drops values whose target is gone", "[model][configuration]") {
  auto b = bracket::make_bracket();
  Document& doc = b.project.document;
  doc.configurations.push_back(config("A", {{b.width, "70"}, {b.upright_load, "3"}}));
  // The Upright goes: its parameter with it.
  std::erase_if(doc.volumes, [&](const Volume& v) { return v.id == b.upright; });
  CHECK(prune_configurations(doc) == 1);
  REQUIRE(doc.configurations[0].values.size() == 1);
  CHECK(doc.configurations[0].values[0].target == b.width);
}

TEST_CASE("Design tables round-trip through CSV", "[model][design_table]") {
  auto [n, inset] = nested_with_variable();
  Document& doc = n.project.document;
  doc.configurations.push_back(config("Big", {{n.w, "60"}, {inset, "2.5"}}));
  doc.configurations.push_back(config("Odd, \"quoted\"", {{n.width, "max(w, 30)"}}));
  doc.active_configuration = doc.configurations[1].id;
  REQUIRE_FALSE(check(doc).has_errors());

  const DesignTable table = design_table(doc);
  REQUIRE(table.columns.size() == 3);
  CHECK(table.columns[0].label == "w");
  CHECK(table.columns[1].label == "Housing.width");
  CHECK(table.columns[2].label == "Housing/inset");
  CHECK(table.columns[1].authored == "w");
  REQUIRE(table.rows.size() == 2);
  CHECK(table.rows[0].cells == std::vector<std::string>{"60", "", "2.5"});
  CHECK(table.rows[1].cells == std::vector<std::string>{"", "max(w, 30)", ""});

  const std::string csv = to_csv(table);
  CHECK(csv ==
        "configuration,w,Housing.width,Housing/inset\n"
        "Big,60,,2.5\n"
        "\"Odd, \"\"quoted\"\"\",,\"max(w, 30)\",\n");
  const auto back = from_csv(doc, csv);
  REQUIRE(back);
  CHECK(*back == table);
  const auto applied = apply_design_table(doc, *back);
  REQUIRE(applied);
  CHECK(*applied == doc);  // ids, values and the active one kept

  // CRLF, spaces and a short row read the same.
  const auto crlf = from_csv(doc,
                             "configuration, w ,Housing.width,Housing/inset\r\n"
                             "Big, 60 ,,2.5\r\n"
                             "\"Odd, \"\"quoted\"\"\",,\"max(w, 30)\"\r\n");
  REQUIRE(crlf);
  CHECK(apply_design_table(doc, *crlf).value() == doc);

  // Every drivable number, for a blank table to fill in.
  CHECK(design_table(doc, true).columns.size() == 3);
  Document none = doc;
  none.configurations.clear();
  none.active_configuration = Id::null();
  CHECK(design_table(none).columns.empty());
  CHECK(design_table(none, true).columns.size() == 3);
}

TEST_CASE("A design table replaces the configurations", "[model][design_table]") {
  auto b = bracket::make_bracket();
  Document& doc = b.project.document;
  doc.configurations.push_back(config("Keep", {{b.width, "70"}}));
  doc.configurations.push_back(config("Drop", {{b.width, "80"}}));
  doc.active_configuration = doc.configurations[1].id;
  const Id keep = doc.configurations[0].id;

  const auto table = from_csv(doc,
                              "configuration,width,Base.thickness\n"
                              "Keep,,4\n"
                              "New,90,\n");
  REQUIRE(table);
  const auto applied = apply_design_table(doc, *table);
  REQUIRE(applied);
  REQUIRE(applied->configurations.size() == 2);
  CHECK(applied->configurations[0].id == keep);
  CHECK(applied->configurations[0].values ==
        std::vector<ConfigurationValue>{{b.base_thickness, "4"}});
  CHECK(applied->configurations[1].name == "New");
  CHECK_FALSE(applied->configurations[1].id.is_null());
  CHECK(applied->active_configuration.is_null());  // "Drop" went
  CHECK_FALSE(check(*applied).has_errors());

  const auto code_of = [&](std::string_view csv) {
    auto t = from_csv(doc, csv);
    if (!t) return t.error().code;
    auto a = apply_design_table(doc, *t);
    return a ? std::string("ok") : a.error().code;
  };
  CHECK(code_of("configuration,nope\nA,1\n") == "model.design_table.column");
  CHECK(code_of("configuration,width,width\nA,1,2\n") == "model.design_table.column");
  CHECK(code_of("configuration,width\n,1\n") == "model.design_table.row");
  CHECK(code_of("configuration,width\nA,1\nA,2\n") == "model.design_table.row");
  CHECK(code_of("configuration,width\nA,1,2\n") == "model.design_table.syntax");
  CHECK(code_of("configuration,width\nA,\"1\n") == "model.design_table.syntax");
  CHECK(code_of("configuration,width\nA,1\"2\"\n") == "model.design_table.syntax");
  CHECK(code_of("") == "model.design_table.syntax");
  CHECK(code_of("configuration,width\nA,70\n") == "ok");
  // Contents aren't columns.
  CHECK(code_of("configuration,Base.nope\nA,1\n") == "model.design_table.column");
}

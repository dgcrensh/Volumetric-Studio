// L5 in the app's rules: configuration commands undo to identical hashes,
// deletions prune values, and what the toolbar, the design table dialog
// and the inspector show (plan docs/plans/L5-configurations-and-design-tables.md,
// test 7).

#include <catch2/catch_test_macros.hpp>

#include "authoring.hpp"
#include "configurations.hpp"
#include "history.hpp"
#include "outline.hpp"
#include "samples.hpp"
#include "vs/model/check.hpp"
#include "vs/model/project.hpp"

using namespace vs;
using app::History;
using core::Id;

namespace {

core::Hash256 hash(const History& h) { return model::content_hash(h.project()); }

Id variable(const model::Document& d, std::string_view name) {
  const auto* v = d.layout.variables().find(name);
  REQUIRE(v != nullptr);
  return v->id;
}

Id parameter(const model::Document& d, std::string_view volume, std::string_view name) {
  const model::Volume* v = d.find_volume(volume);
  REQUIRE(v != nullptr);
  const auto* p = v->find_parameter(name);
  REQUIRE(p != nullptr);
  return p->id;
}

void apply(History& h, const app::Command& c) {
  const auto d = h.apply(c);
  INFO(samples::codes(d));
  REQUIRE_FALSE(d.has_errors());
}

}  // namespace

TEST_CASE("Configuration commands undo to identical hashes", "[app][configuration]") {
  History h(samples::load("bracket.vsproj"));
  const auto h0 = hash(h);
  const model::Document& doc0 = h.project().document;
  const Id width = variable(doc0, "width");
  const Id thickness = parameter(doc0, "Base", "thickness");

  const Id thin = Id::random();
  apply(h, app::add_configuration("Thin", {}, true, thin));
  CHECK(h.project().document.active_configuration == thin);
  apply(h, app::set_configuration_value(thin, thickness, "6"));
  apply(h, app::set_configuration_value(thin, width, "70"));
  apply(h, app::set_configuration_value(thin, width, "72"));  // replaces
  REQUIRE(h.project().document.configurations[0].values.size() == 2);
  CHECK(h.project().document.configurations[0].find(width)->expression == "72");

  // A copy, made unique, not active.
  apply(h, app::add_configuration("Thin", thin, false));
  const auto& configs = h.project().document.configurations;
  REQUIRE(configs.size() == 2);
  CHECK(configs[1].name == "Thin 2");
  CHECK(configs[1].values == configs[0].values);
  CHECK(h.project().document.active_configuration == thin);

  apply(h, app::rename_configuration(configs[1].id, "Wide"));
  apply(h, app::set_configuration_value(configs[1].id, thickness, ""));  // clears
  CHECK(h.project().document.configurations[1].values.size() == 1);
  apply(h, app::set_active_configuration(h.project().document.configurations[1].id));
  apply(h, app::set_active_configuration(Id::null()));
  const auto h_full = hash(h);

  // Refused: a duplicate name, a bad expression, something not drivable.
  CHECK(h.apply(app::rename_configuration(thin, "Wide")).has_errors());
  CHECK(h.apply(app::set_configuration_value(thin, width, "70 +")).has_errors());
  CHECK(h.apply(app::set_configuration_value(thin, h.project().document.volumes[0].id, "1"))
            .has_errors());
  CHECK(h.apply(app::set_active_configuration(Id::random())).has_errors());
  CHECK(hash(h) == h_full);

  apply(h, app::remove_configuration(thin));
  CHECK(h.project().document.configurations.size() == 1);
  REQUIRE(h.undo());
  CHECK(hash(h) == h_full);
  while (h.can_undo()) REQUIRE(h.undo());
  CHECK(hash(h) == h0);
  while (h.can_redo()) REQUIRE(h.redo());
  CHECK(h.project().document.configurations.size() == 1);
}

TEST_CASE("Deleting what a configuration drives drops its values", "[app][configuration]") {
  History h(samples::load("bracket.vsproj"));
  const model::Document& doc = h.project().document;
  const Id load = parameter(doc, "Upright", "load");
  const Id height = variable(doc, "upright_h");
  const Id c = Id::random();
  apply(h, app::add_configuration("Tall", {}, true, c));
  apply(h, app::set_configuration_value(c, height, "45"));
  apply(h, app::set_configuration_value(c, load, "3"));

  // The Upright goes, and its parameter's value with it.
  apply(h, app::remove_volume(h.project().document.find_volume("Upright")->id));
  REQUIRE(h.project().document.configurations[0].values.size() == 1);
  CHECK(h.project().document.configurations[0].values[0].target == height);
  CHECK_FALSE(model::check(h.project().document).has_errors());
  REQUIRE(h.undo());
  CHECK(h.project().document.configurations[0].values.size() == 2);
}

TEST_CASE("The toolbar, the design table and the inspector", "[app][configuration]") {
  History h(samples::load("bracket.vsproj"));
  const model::Document& doc = h.project().document;
  const Id width = variable(doc, "width");
  const Id thickness = parameter(doc, "Base", "thickness");
  const Id a = Id::random();
  apply(h, app::add_configuration("Thin", {}, false, a));
  apply(h, app::set_configuration_value(a, thickness, "6"));

  auto choices = app::configuration_choices(h.project().document);
  REQUIRE(choices.size() == 2);
  CHECK(choices[0].id.is_null());
  CHECK(choices[0].active);
  CHECK(choices[1].label == "Thin");
  CHECK_FALSE(app::active_value(h.project().document, thickness));
  apply(h, app::set_active_configuration(a));
  choices = app::configuration_choices(h.project().document);
  CHECK(choices[1].active);
  CHECK(app::active_value(h.project().document, thickness) == "6");
  CHECK_FALSE(app::active_value(h.project().document, width));

  // The dialog's grid: driven columns plus the ones the user added.
  auto table = app::table_for_editing(h.project().document);
  REQUIRE(table.columns.size() == 1);
  CHECK(table.columns[0].label == "Base.thickness");
  const auto addable = app::addable_columns(h.project().document, table);
  CHECK(std::any_of(addable.begin(), addable.end(),
                    [&](const model::Drivable& d) { return d.label == "width"; }));
  table = app::table_for_editing(h.project().document, {width});
  REQUIRE(table.columns.size() == 2);
  CHECK(table.columns[0].label == "width");  // drivable order
  table.rows[0].cells[0] = "80";
  table.rows.push_back({Id::null(), "Wide", {"90", ""}});
  apply(h, app::set_design_table(table));
  REQUIRE(h.project().document.configurations.size() == 2);
  CHECK(h.project().document.configurations[0].id == a);
  CHECK(h.project().document.configurations[0].find(width)->expression == "80");
  // A table that names a configuration twice is refused, nothing changes.
  const auto before = hash(h);
  table.rows.push_back({Id::null(), "Wide", {"95", ""}});
  CHECK(h.apply(app::set_design_table(table)).has_errors());
  CHECK(hash(h) == before);

  // The evaluation shows the active configuration and its values.
  const auto ev = samples::evaluate(h.project());
  INFO(samples::codes(ev.diagnostics));
  REQUIRE(ev.ok());
  const auto props = app::inspect(ev, {});
  CHECK(std::any_of(props.begin(), props.end(), [](const app::Property& p) {
    return p.name == "Configuration" && p.value == "Thin";
  }));
  CHECK(std::any_of(props.begin(), props.end(), [](const app::Property& p) {
    return p.name == "width" && p.value.find("80") != std::string::npos;
  }));
}

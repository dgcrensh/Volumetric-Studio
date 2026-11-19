// The panels' contents: selection, routing, tree rows and the inspector.

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "outline.hpp"
#include "samples.hpp"

using namespace vs;
using app::Selection;

namespace {

const core::Diagnostic* first_with(const core::Diagnostics& d, std::string_view code) {
  const auto found = d.with_code(code);
  return found.empty() ? nullptr : found.front();
}

bool has_property(const std::vector<app::Property>& props, std::string_view group, std::string_view name) {
  return std::any_of(props.begin(), props.end(),
                     [&](const app::Property& p) { return p.group == group && p.name == name; });
}

std::string value_of(const std::vector<app::Property>& props, std::string_view group, std::string_view name) {
  for (const auto& p : props) {
    if (p.group == group && p.name == name) return p.value;
  }
  return {};
}

}  // namespace

TEST_CASE("Clicking a diagnostic selects its act-here target", "[app][outline][acceptance]") {
  SECTION("an overlap selects the first volume") {
    const auto ev = samples::evaluate(samples::load("overlap.vsproj"));
    const auto* d = first_with(ev.diagnostics, "validate.overlap");
    REQUIRE(d != nullptr);
    const Selection s = app::selection_for(*d, ev.document);
    CHECK(s == Selection::of_volume(ev.document.volumes[0].id));
    CHECK(app::viewport_subject(s) == ev.document.volumes[0].id);
  }
  SECTION("a connection rule selects the surface") {
    const auto ev = samples::evaluate(samples::load("mismatch.vsproj"));
    const core::Diagnostic* d = nullptr;
    for (const auto& item : ev.diagnostics) {
      if (item.code.starts_with("validate.connection.")) d = &item;
    }
    REQUIRE(d != nullptr);
    const Selection s = app::selection_for(*d, ev.document);
    CHECK(s.kind == Selection::Kind::ConnectionSurface);
    CHECK(s.id == ev.document.connections[0].id);
    CHECK(app::viewport_subject(s) == ev.document.connections[0].id);
  }
  SECTION("contents sticking out select the volume") {
    const auto ev = samples::evaluate(samples::load("containment.vsproj"));
    const auto* d = first_with(ev.diagnostics, "validate.containment");
    REQUIRE(d != nullptr);
    CHECK(app::selection_for(*d, ev.document) == Selection::of_volume(ev.document.volumes[0].id));
  }
  SECTION("a dependency loop acts on the layout") {
    const auto ev = samples::evaluate(samples::load("loop.vsproj"));
    INFO(samples::codes(ev.diagnostics));
    REQUIRE(ev.diagnostics.has_errors());
    const auto& d = ev.diagnostics[0];
    const Selection s = app::selection_for(d, ev.document);
    // Whatever it names, it never selects something that doesn't exist.
    if (s.kind == Selection::Kind::Volume) CHECK(ev.document.find_volume(s.id) != nullptr);
  }
  SECTION("a failed feature in a volume selects that feature") {
    core::Diagnostic d = core::Diagnostic::error("feature.extrude.failed", "x");
    const auto ev = samples::evaluate(samples::load("bracket.vsproj"));
    const model::Volume* parametric = nullptr;
    for (const auto& v : ev.document.volumes) {
      if (v.tree() != nullptr && !v.tree()->features().empty()) parametric = &v;
    }
    REQUIRE(parametric != nullptr);
    const auto& f = parametric->tree()->features().back();
    d.subjects.push_back({core::SubjectKind::Feature, f.id, "f"});
    const Selection s = app::selection_for(d, ev.document);
    CHECK(s == Selection::of_feature(f.id, parametric->id));
    CHECK(app::viewport_subject(s) == parametric->id);
  }
}

TEST_CASE("The volume tree lists kinds and statuses", "[app][outline]") {
  const auto ev = samples::evaluate(samples::load("bracket.vsproj"));
  INFO(samples::codes(ev.diagnostics));
  const auto rows = app::volume_rows(ev);
  REQUIRE(rows.size() == ev.document.volumes.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    CHECK(rows[i].id == ev.document.volumes[i].id);
    CHECK(rows[i].status == ev.volumes[i].status);
    CHECK(!app::kind_label(rows[i]).empty());
  }
  // The engine's live states win while it runs.
  std::map<core::Id, model::VolumeStatus> live{{rows[0].id, model::VolumeStatus::Regenerating}};
  CHECK(app::volume_rows(ev, live)[0].status == model::VolumeStatus::Regenerating);
  CHECK(app::status_label(model::VolumeStatus::Regenerating) == "Regenerating");

  const auto surfaces = app::surface_rows(ev);
  REQUIRE(surfaces.size() == ev.document.connections.size());
  for (const auto& s : surfaces) {
    CHECK(s.ok);
    CHECK(!s.volumes.empty());
  }
}

TEST_CASE("The inspector shows parameters, published outputs and surface properties", "[app][outline]") {
  const auto ev = samples::evaluate(samples::load("bracket.vsproj"));
  REQUIRE(ev.ok());
  bool saw_output = false, saw_parameter = false;
  for (const auto& v : ev.document.volumes) {
    const auto props = app::inspect(ev, Selection::of_volume(v.id));
    CHECK(value_of(props, "Volume", "Name") == v.name);
    CHECK(value_of(props, "Volume", "Status") == "Current");
    for (const auto& p : v.parameters) {
      CHECK(has_property(props, "Parameters", p.name));
      saw_parameter = true;
    }
    for (const auto& o : v.outputs) {
      const std::string label = o.name + " (" + std::string(model::to_string(o.quantity)) + ")";
      const auto value = value_of(props, "Published outputs", label);
      CHECK(!value.empty());
      CHECK(value != "-");  // measured
      saw_output = true;
    }
  }
  CHECK(saw_parameter);
  CHECK(saw_output);

  REQUIRE(!ev.document.connections.empty());
  for (const auto& s : ev.document.connections) {
    const auto props = app::inspect(ev, Selection::of_surface(s.id));
    CHECK(value_of(props, "Connection surface", "Type") == model::to_string(s.type()));
    CHECK(has_property(props, "Connection surface", "Area"));
    if (s.type() == model::ConnectionType::BoltedFlange) {
      CHECK(has_property(props, "Bolted flange", "Thread"));
      CHECK(has_property(props, "Bolted flange", "Hole centres"));
    }
  }

  // Nothing selected: the project and its variables.
  const auto project = app::inspect(ev, Selection::none());
  CHECK(value_of(project, "Project", "Name") == ev.document.name);
}

TEST_CASE("The feature tree shows layout features, a volume's features or its graph nodes", "[app][outline]") {
  const auto bracket = samples::evaluate(samples::load("bracket.vsproj"));
  const auto layout = app::feature_rows(bracket, {});
  CHECK(layout.size() == bracket.document.layout.features().size());
  for (const auto& row : layout) {
    CHECK(row.selection.kind == Selection::Kind::Feature);
    CHECK(row.status == "ok");
  }
  for (const auto& v : bracket.document.volumes) {
    const auto rows = app::feature_rows(bracket, v.id);
    if (const auto* tree = v.tree()) {
      CHECK(rows.size() == tree->features().size());
      if (!rows.empty()) {
        const auto props = app::inspect(bracket, rows.front().selection);
        CHECK(value_of(props, "Feature", "In volume") == v.name);
      }
    }
  }

  const auto ball = samples::evaluate(samples::load("shell_and_ball.vsproj"));
  const auto& v = ball.document.volumes.at(0);
  const auto nodes = app::feature_rows(ball, v.id);
  REQUIRE(nodes.size() == 6);
  const auto shell = std::find_if(nodes.begin(), nodes.end(), [](const auto& r) { return r.kind == "field.shell"; });
  REQUIRE(shell != nodes.end());
  CHECK(shell->selection.kind == Selection::Kind::Node);
  CHECK(app::viewport_subject(shell->selection) == v.id);
  const auto props = app::inspect(ball, shell->selection);
  CHECK(value_of(props, "Node", "Type") == "field.shell");
}

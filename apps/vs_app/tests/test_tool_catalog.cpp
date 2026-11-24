// U1's rules, headless (decision 0039): the tool catalogue and its icons,
// which context the window is in, which tabs it can switch to, and which
// tools can act on the selection.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>

#include "authoring.hpp"
#include "icons.hpp"
#include "samples.hpp"
#include "tool_catalog.hpp"

using namespace vs;
using namespace vs::app;
using core::Id;

namespace {

const model::Volume& volume_named(const model::Document& doc, const std::string& name) {
  const auto* v = doc.find_volume(name);
  REQUIRE(v != nullptr);
  return *v;
}

// The first volume whose contents are of `kind`.
const model::Volume* first_of(const model::Document& doc, ContentsKind kind) {
  for (const auto& v : doc.volumes) {
    if (contents_kind(v) == kind) return &v;
  }
  return nullptr;
}

// The first volume with imported static contents (none of the kinds).
const model::Volume* first_imported(const model::Document& doc) {
  for (const auto& v : doc.volumes) {
    if (!v.keep_out && !contents_kind(v)) return &v;
  }
  return nullptr;
}

ToolState selecting(const Id& volume) {
  ToolState s;
  s.selection = Selection::of_volume(volume);
  return s;
}

// A feature of `kind` in `tree`.
const feature::Feature* first_feature(const feature::FeatureTree& tree, feature::FeatureKind kind) {
  for (const auto& f : tree.features()) {
    if (f.kind() == kind) return &f;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("Every tool is one catalogue row with a name, label, tooltip and icon", "[app][tools]") {
  const auto tools = all_tools();
  std::set<std::string_view> names;
  for (std::size_t i = 0; i < tools.size(); ++i) {
    const Tool& t = tools[i];
    INFO(t.name);
    CHECK(static_cast<std::size_t>(t.id) == i);  // in ToolId order: tool() indexes by id
    CHECK(&tool(t.id) == &t);
    CHECK(!t.name.empty());
    CHECK(!t.label.empty());
    CHECK(!t.tooltip.empty());
    CHECK(names.insert(t.name).second);
    CHECK(find_tool(t.name) == t.id);
    CHECK(!icon_strokes(t.icon).empty());
    // Every tool is on some context's toolbar.
    CHECK(std::any_of(std::begin(kContexts), std::end(kContexts), [&](Context c) { return shows_in(t.id, c); }));
  }
  CHECK(tools.back().id == ToolId::CloseDrawing);
  CHECK_FALSE(find_tool("no_such_tool").has_value());
}

TEST_CASE("Shortcuts are unique, and the window's own keys stay free", "[app][tools]") {
  // The window and sketch mode keep these (H7: one holder per shortcut).
  const std::set<std::string_view> window = {"Ctrl+Z", "Ctrl+Y", "Ctrl+N", "Ctrl+O", "Ctrl+S", "Ctrl+Shift+S", "Ctrl+Q", "Ctrl+I",
                                             "F", "F5", "Esc", "Del", "Return", "Enter"};
  std::set<std::string_view> seen;
  for (const Tool& t : all_tools()) {
    if (t.shortcut.empty()) continue;
    INFO(t.name << " " << t.shortcut);
    CHECK(seen.insert(t.shortcut).second);
    CHECK(window.count(t.shortcut) == 0);
  }
  // The menus' shortcuts carried over, but Sketch: Ctrl+Shift+S is Save As.
  CHECK(tool(ToolId::Sketch).shortcut == "Ctrl+Shift+K");
  CHECK(tool(ToolId::Extrude).shortcut == "Ctrl+Shift+E");
  CHECK(tool(ToolId::EditContents).shortcut == "Ctrl+E");
  CHECK(tool(ToolId::EditGraph).shortcut == "Ctrl+G");
  CHECK(tool(ToolId::Validate).shortcut == "Ctrl+R");
}

TEST_CASE("Each context's toolbar holds named groups of distinct tools", "[app][tools]") {
  for (Context c : kContexts) {
    INFO(to_string(c));
    CHECK(!title(c).empty());
    const auto& groups = tool_groups(c);
    REQUIRE(!groups.empty());
    std::set<ToolId> seen;
    for (const ToolGroup& g : groups) {
      CHECK(!g.name.empty());
      CHECK(!g.tools.empty());
      CHECK(g.shown >= 1);
      for (ToolId id : g.tools) CHECK(seen.insert(id).second);
    }
  }
  // Every feature kind and every constraint kind has exactly one tool.
  for (int k = 0; k <= static_cast<int>(feature::FeatureKind::CircularPattern); ++k) {
    const auto kind = static_cast<feature::FeatureKind>(k);
    const auto n = std::count_if(all_tools().begin(), all_tools().end(),
                                 [&](const Tool& t) { return feature_kind(t.id) == kind; });
    CHECK(n == 1);
  }
  for (int k = 0; k <= static_cast<int>(sketch::ConstraintKind::Fix); ++k) {
    const auto kind = static_cast<sketch::ConstraintKind>(k);
    const auto n = std::count_if(all_tools().begin(), all_tools().end(),
                                 [&](const Tool& t) { return constraint_kind(t.id) == kind; });
    CHECK(n == 1);
    // All of them on the sketch toolbar.
    const auto id = std::find_if(all_tools().begin(), all_tools().end(),
                                 [&](const Tool& t) { return constraint_kind(t.id) == kind; });
    CHECK(shows_in(id->id, Context::Sketch));
  }
  // Modelled contents get every feature; so does the layout's tree.
  for (const Tool& t : all_tools()) {
    if (!feature_kind(t.id)) continue;
    INFO(t.name);
    CHECK(shows_in(t.id, Context::Contents));
    CHECK(shows_in(t.id, Context::Layout));
  }
}

TEST_CASE("Icons are strokes on the 24-unit grid", "[app][tools][icons]") {
  const auto icons = all_icons();
  std::set<std::string_view> names;
  for (std::size_t i = 0; i < icons.size(); ++i) {
    const Icon icon = icons[i];
    INFO(to_string(icon));
    CHECK(static_cast<std::size_t>(icon) == i);
    CHECK(names.insert(to_string(icon)).second);
    const auto& strokes = icon_strokes(icon);
    REQUIRE(!strokes.empty());
    for (const IconStroke& s : strokes) {
      if (s.dot) {
        CHECK(s.points.size() == 1);
      } else {
        CHECK(s.points.size() >= 2);
      }
      for (const IconPoint& p : s.points) {
        CHECK(p.x >= 0.0);
        CHECK(p.x <= kIconGrid);
        CHECK(p.y >= 0.0);
        CHECK(p.y <= kIconGrid);
      }
    }
  }
  CHECK(to_string(Icon::Variables) == "variables");
  // Same strokes on every call.
  CHECK(&icon_strokes(Icon::Volume) == &icon_strokes(Icon::Volume));
}

TEST_CASE("The context follows what the window has open", "[app][tools]") {
  const model::Project nested = samples::load("nested.vsproj");
  const auto& doc = nested.document;
  const model::Volume* leaf = first_of(doc, ContentsKind::Parametric);
  const model::Volume* composite = first_of(doc, ContentsKind::Composite);
  REQUIRE(leaf != nullptr);
  REQUIRE(composite != nullptr);

  ToolState s;
  CHECK(current_context(doc, s) == Context::Layout);
  s.editing_volume = composite->id;  // a composite's layout is a layout
  CHECK(current_context(doc, s) == Context::Layout);
  s.editing_volume = leaf->id;
  CHECK(current_context(doc, s) == Context::Contents);
  s.graph_volume = leaf->id;
  CHECK(current_context(doc, s) == Context::Graph);
  s.drawing_shown = true;
  CHECK(current_context(doc, s) == Context::Drawing);
  s.sketch_open = true;  // sketch mode wins over everything
  CHECK(current_context(doc, s) == Context::Sketch);
  // A volume that no longer exists is no context.
  ToolState gone;
  gone.editing_volume = Id::random();
  CHECK(current_context(doc, gone) == Context::Layout);
}

TEST_CASE("Tabs say whether they can be entered from here, and why not", "[app][tools]") {
  const model::Project bracket = samples::load("bracket.vsproj");
  const auto& doc = bracket.document;
  const model::Volume* modelled = first_of(doc, ContentsKind::Parametric);
  REQUIRE(modelled != nullptr);
  const model::Project imported = samples::static_mesh();
  const model::Volume* mesh = first_imported(imported.document);
  REQUIRE(mesh != nullptr);

  ToolState none;
  CHECK(tab_availability(Context::Layout, doc, none));
  const auto contents = tab_availability(Context::Contents, doc, none);
  CHECK_FALSE(contents);
  CHECK(!contents.reason.empty());
  CHECK(tab_availability(Context::Contents, doc, selecting(modelled->id)));
  CHECK_FALSE(tab_availability(Context::Contents, imported.document, selecting(mesh->id)));
  CHECK_FALSE(tab_availability(Context::Graph, doc, selecting(modelled->id)));  // modelled, not procedural
  CHECK_FALSE(tab_availability(Context::Sketch, doc, selecting(modelled->id)));

  // A generated volume is read-only.
  ToolState generated = selecting(modelled->id);
  generated.selection_generated = true;
  CHECK_FALSE(tab_availability(Context::Contents, doc, generated));

  // Selecting a sketch feature opens the Sketch tab.
  const feature::Feature* sketch = first_feature(*modelled->tree(), feature::FeatureKind::Sketch);
  REQUIRE(sketch != nullptr);
  ToolState on_sketch;
  on_sketch.selection = Selection::of_feature(sketch->id, modelled->id);
  CHECK(selection_is_sketch(doc, on_sketch.selection));
  CHECK(tab_availability(Context::Sketch, doc, on_sketch));
  const feature::Feature* layout_sketch = first_feature(doc.layout, feature::FeatureKind::Sketch);
  REQUIRE(layout_sketch != nullptr);
  CHECK(selection_is_sketch(doc, Selection::of_feature(layout_sketch->id)));
  CHECK_FALSE(selection_is_sketch(doc, Selection::of_volume(modelled->id)));

  // Drawings need something to draw.
  CHECK_FALSE(tab_availability(Context::Drawing, doc, none));
  ToolState evaluated;
  evaluated.evaluated = true;
  CHECK(tab_availability(Context::Drawing, doc, evaluated));

  // While sketching, only Sketch.
  ToolState sketching = on_sketch;
  sketching.sketch_open = true;
  for (Context c : kContexts) {
    INFO(to_string(c));
    const auto a = tab_availability(c, doc, sketching);
    CHECK(a.enabled == (c == Context::Sketch));
    if (!a) CHECK(a.reason == "Finish or cancel the sketch first.");
  }
}

TEST_CASE("Fill tools are enabled only for contents they can act on", "[app][tools]") {
  model::Project project = samples::load("bracket.vsproj");
  auto& doc = project.document;
  const Id modelled = first_of(doc, ContentsKind::Parametric)->id;
  const model::Project imported = samples::static_mesh();
  const Id imported_volume = first_imported(imported.document)->id;

  // An empty volume and a procedural one, made the app's way.
  const Id empty = volume_named(doc, "Upright").id;
  REQUIRE(!swap_contents(empty, ContentsKind::Placeholder).change(project).has_errors());
  const Id procedural = modelled;
  model::Project with_procedure = project;
  REQUIRE(!swap_contents(procedural, ContentsKind::Procedural).change(with_procedure).has_errors());

  auto on = [&](const model::Project& p, const Id& v, ToolId id) { return availability(id, p.document, selecting(v)); };

  // Nothing selected: every fill tool says to select a volume.
  for (ToolId id : {ToolId::EditContents, ToolId::EditGraph, ToolId::AttachTemplate, ToolId::ImportContents,
                    ToolId::Bake, ToolId::Unbake, ToolId::MakeComposite, ToolId::GenerateChildren}) {
    const auto a = availability(id, doc, ToolState{});
    CHECK_FALSE(a);
    CHECK(a.reason == "Select a volume first.");
  }
  // Creating needs no selection.
  for (ToolId id : {ToolId::NewVolume, ToolId::NewConnection, ToolId::NewKeepOut, ToolId::NewLink, ToolId::Sketch,
                    ToolId::Validate}) {
    CHECK(availability(id, doc, ToolState{}));
  }

  // Modelled contents.
  CHECK(on(project, modelled, ToolId::EditContents));
  CHECK_FALSE(on(project, modelled, ToolId::EditGraph));
  CHECK_FALSE(on(project, modelled, ToolId::AttachTemplate));
  CHECK_FALSE(on(project, modelled, ToolId::Bake));
  CHECK(on(project, modelled, ToolId::MakeComposite));
  CHECK_FALSE(on(project, modelled, ToolId::GenerateChildren));
  // An empty volume can be filled any way.
  for (ToolId id : {ToolId::EditContents, ToolId::EditGraph, ToolId::AttachTemplate, ToolId::ImportContents}) {
    CHECK(on(project, empty, id));
  }
  CHECK_FALSE(on(project, empty, ToolId::Bake));
  // Imported static contents: only import (again) or restructure.
  CHECK_FALSE(on(imported, imported_volume, ToolId::EditContents));
  CHECK_FALSE(on(imported, imported_volume, ToolId::EditGraph));
  CHECK(on(imported, imported_volume, ToolId::ImportContents));
  // A procedure.
  CHECK(on(with_procedure, procedural, ToolId::EditGraph));
  CHECK(on(with_procedure, procedural, ToolId::AttachTemplate));
  CHECK(on(with_procedure, procedural, ToolId::Bake));
  CHECK_FALSE(on(with_procedure, procedural, ToolId::Unbake));
  CHECK_FALSE(on(with_procedure, procedural, ToolId::EditContents));

  // Generated volumes are read-only.
  ToolState generated = selecting(empty);
  generated.selection_generated = true;
  CHECK_FALSE(availability(ToolId::EditContents, doc, generated));
  CHECK_FALSE(availability(ToolId::ImportContents, doc, generated));

  // Tools of another context aren't enabled here.
  const auto line = availability(ToolId::SketchLine, doc, ToolState{});
  CHECK_FALSE(line);
  CHECK(line.reason == "Not available in Layout.");
  CHECK_FALSE(availability(ToolId::LeaveContext, doc, ToolState{}));
}

TEST_CASE("Composites: their layout is edited in Layout, their children generated", "[app][tools]") {
  const model::Project nested = samples::load("nested.vsproj");
  const auto& doc = nested.document;
  const model::Volume* composite = first_of(doc, ContentsKind::Composite);
  REQUIRE(composite != nullptr);
  const auto s = selecting(composite->id);
  CHECK(availability(ToolId::GenerateChildren, doc, s));
  CHECK(availability(ToolId::EditContents, doc, s));  // edits its layout in context
  CHECK_FALSE(availability(ToolId::MakeComposite, doc, s));
  CHECK_FALSE(availability(ToolId::ImportContents, doc, s));
  CHECK_FALSE(tab_availability(Context::Contents, doc, s));
}

TEST_CASE("Sketch tools follow the sketch selection", "[app][tools]") {
  const model::Project bracket = samples::load("bracket.vsproj");
  const auto& doc = bracket.document;
  ToolState s;
  s.sketch_open = true;
  REQUIRE(current_context(doc, s) == Context::Sketch);

  // Nothing selected: drawing works, constraining waits.
  CHECK(availability(ToolId::SketchLine, doc, s));
  CHECK(availability(ToolId::FinishSketch, doc, s));
  CHECK(availability(ToolId::CancelSketch, doc, s));
  CHECK_FALSE(availability(ToolId::Construction, doc, s));
  CHECK_FALSE(availability(ToolId::DeleteSketchItems, doc, s));
  const auto h = availability(ToolId::Horizontal, doc, s);
  CHECK_FALSE(h);
  CHECK(h.reason == "Select the geometry to constrain first.");

  const auto offset = availability(ToolId::OffsetCurves, doc, s);
  CHECK_FALSE(offset);
  CHECK(offset.reason == "Select the curves to offset first.");
  CHECK(shows_in(ToolId::OffsetCurves, Context::Sketch));

  // A point alone can't be offset.
  s.sketch_selection = 1;
  CHECK_FALSE(availability(ToolId::OffsetCurves, doc, s));

  // One line selected: what fits it.
  s.sketch_curve_selection = 1;
  s.applicable_constraints = {sketch::ConstraintKind::Horizontal, sketch::ConstraintKind::Vertical,
                              sketch::ConstraintKind::Distance, sketch::ConstraintKind::Angle,
                              sketch::ConstraintKind::Fix};
  CHECK(availability(ToolId::Horizontal, doc, s));
  CHECK(availability(ToolId::Distance, doc, s));
  CHECK(availability(ToolId::Construction, doc, s));
  CHECK(availability(ToolId::OffsetCurves, doc, s));
  const auto tangent = availability(ToolId::Tangent, doc, s);
  CHECK_FALSE(tangent);
  CHECK(tangent.reason == "Doesn't fit the selected geometry.");
  // Layout tools wait for the sketch.
  CHECK_FALSE(availability(ToolId::NewVolume, doc, s));
}

TEST_CASE("Graph and drawing tools act on what is open", "[app][tools]") {
  model::Project project = samples::load("bracket.vsproj");
  const Id v = first_of(project.document, ContentsKind::Parametric)->id;
  REQUIRE(!swap_contents(v, ContentsKind::Procedural).change(project).has_errors());
  const auto& doc = project.document;

  ToolState graph;
  graph.graph_volume = v;
  REQUIRE(current_context(doc, graph) == Context::Graph);
  CHECK(availability(ToolId::Bake, doc, graph));  // the open graph's volume, whatever is selected
  CHECK(availability(ToolId::AttachTemplate, doc, graph));
  CHECK(availability(ToolId::LeaveGraph, doc, graph));
  CHECK_FALSE(availability(ToolId::NewVolume, doc, graph));

  ToolState drawing;
  drawing.drawing_shown = true;
  REQUIRE(current_context(doc, drawing) == Context::Drawing);
  CHECK_FALSE(availability(ToolId::NewDrawing, doc, drawing));
  drawing.evaluated = true;
  CHECK(availability(ToolId::NewDrawing, doc, drawing));
  CHECK(availability(ToolId::AddDrawingView, doc, drawing));
  CHECK(availability(ToolId::ExportDrawing, doc, drawing));
  CHECK(availability(ToolId::CloseDrawing, doc, drawing));
}

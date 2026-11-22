// L4: generated children in the app. An authored child becomes a
// template, a generator fills the composite from it through the same
// commands as any graph edit, undo restores identical hashes, the outline
// nests the generated rows, and they are read-only.

#include <QApplication>
#include <QTreeWidget>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "authoring.hpp"
#include "history.hpp"
#include "main_window.hpp"
#include "outline.hpp"
#include "rack.hpp"
#include "samples.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/check.hpp"
#include "vs/model/hierarchy.hpp"
#include "vs/model/project.hpp"
#include "vs/proc/generator.hpp"
#include "wait.hpp"

using namespace vs;
using app::History;
using core::Id;
using feature::FeatureRef;
using feature::Param;

namespace {

const auto kSettings = core::ProductSettings::defaults();

// The rack with no generator and no template, and one authored child Cell
// (box 0..20 x 0..20 x 0..10, filled by the rack's cell tree) to make the
// template from.
struct Authored {
  model::Project project;
  Id rack, cell;
};

Authored authored_rack() {
  rack::Rack r = rack::make_rack("");
  model::Document& doc = r.project.document;
  model::Volume* rack_volume = doc.find_volume(r.rack);
  auto& comp = std::get<model::CompositeContents>(rack_volume->contents);
  comp.templates.clear();
  const Id lo = comp.layout.add(feature::DatumPointFeature{feature::PointAt{Param::of(0), Param::of(0), Param::of(0)}},
                                "Cell low");
  const Id hi = comp.layout.add(
      feature::DatumPointFeature{feature::PointAt{Param::of(20), Param::of(20), Param::of(10)}}, "Cell high");
  model::Volume cell = app::volume_from_box("Cell", FeatureRef{lo}, FeatureRef{hi}, r.rack);
  cell.contents = rack::cell_tree(cell.id, {20, 20, 10});
  cell.parameters.push_back({Id::random(), "depth", Param::of(10)});
  comp.delegations.push_back({r.seat, cell.id});
  const Id cell_id = cell.id;
  doc.volumes.push_back(std::move(cell));
  REQUIRE_FALSE(model::check(doc).has_errors());
  return {r.project, r.rack, cell_id};
}

eval::Evaluation evaluate(const model::Project& project) {
  eval::Engine engine(kSettings);
  proc::install(engine);
  return engine.evaluate(project);
}

}  // namespace

TEST_CASE("Make template, then a generator fills the composite from it", "[app][authoring][generated]") {
  const Authored a = authored_rack();
  History h(a.project);
  const auto initial = model::content_hash(h.project());

  REQUIRE_FALSE(h.apply(app::make_template(a.cell, {0, 0, 0})).has_errors());
  const model::Document& doc = h.project().document;
  CHECK(doc.find_volume(a.cell) == nullptr);
  const auto& comp = std::get<model::CompositeContents>(doc.find_volume(a.rack)->contents);
  REQUIRE(comp.templates.size() == 1);
  CHECK(comp.templates[0].id == a.cell);  // its tree's references stay valid
  CHECK(comp.templates[0].name == "Cell");
  CHECK(comp.delegations.empty());  // the delegation went with the child
  const auto templated = model::content_hash(h.project());

  // Only children of composites, and not composites, make templates.
  CHECK(h.apply(app::make_template(a.rack, {})).has_errors());

  // A grid of three cells from it, the seat passed to the first.
  proc::Graph g = app::starter_generator("Cell");
  proc::Node* grid = nullptr;
  for (auto& n : g.nodes) {
    if (n.type == "children.grid") grid = &n;
  }
  REQUIRE(grid != nullptr);
  grid->settings["count"] = proc::Setting(core::dvec3{3, 2, 1});
  REQUIRE_FALSE(h.apply(app::set_graph(a.rack, g, "Generate children")).has_errors());
  REQUIRE(app::graph_of(h.project().document, a.rack));
  CHECK(app::graph_of(h.project().document, a.rack)->nodes.size() == 2);

  // A graph naming a template the composite doesn't have is refused.
  proc::Graph wrong = g;
  for (auto& n : wrong.nodes) {
    if (n.type == "children.grid") n.settings["template"] = proc::Setting(std::string("Bracket"));
  }
  CHECK(h.apply(app::set_graph(a.rack, wrong, "Wrong")).has_errors());

  const auto ev = evaluate(h.project());
  for (const auto& d : ev.diagnostics) INFO(d.to_string());
  std::size_t generated = 0;
  for (const auto& v : ev.document.volumes) generated += v.generated ? 1 : 0;
  CHECK(generated == 6);
  CHECK(ev.find(a.rack)->status == model::VolumeStatus::Current);

  // The outline nests them under the Rack, marked generated.
  const auto rows = app::volume_rows(ev);
  std::size_t nested = 0;
  for (const auto& row : rows) {
    if (!row.generated) continue;
    ++nested;
    CHECK(row.parent == a.rack);
    CHECK(row.depth == 1);
    CHECK(app::kind_label(row) == "Generated Parametric");
  }
  CHECK(nested == 6);

  // Undo restores identical hashes, step by step.
  REQUIRE(h.undo());
  CHECK(model::content_hash(h.project()) == templated);
  REQUIRE(h.undo());
  CHECK(model::content_hash(h.project()) == initial);
}

TEST_CASE("Generated volumes are read-only in the window", "[app][gui][generated]") {
  rack::Rack r = rack::make_rack();
  auto& comp = std::get<model::CompositeContents>(r.project.document.find_volume(r.rack)->contents);
  comp.generator = proc::to_json(app::starter_generator("Cell"));
  const auto dir =
      std::filesystem::temp_directory_path() / ("vstudio-generated-" + Id::random().to_string() + ".vsproj");
  REQUIRE_FALSE(model::save_project(r.project, dir, model::ProjectLayout::Folder).has_errors());

  app::MainWindow w;
  w.resize(1280, 800);
  w.show();
  REQUIRE(w.open_project(dir));
  REQUIRE(samples::wait_until(
      [&] {
        const auto s = w.session().latest();
        return !w.session().busy() && s && !s->preview && s->project == w.session().project_ptr();
      },
      120000));
  QApplication::processEvents();

  const auto items = w.volume_tree().findItems(QStringLiteral("Cell 1"), Qt::MatchExactly | Qt::MatchRecursive);
  REQUIRE(items.size() == 1);
  REQUIRE(items.front()->parent() != nullptr);
  CHECK(items.front()->parent()->text(0) == QStringLiteral("Rack"));

  const auto s = w.session().latest();
  Id cell;
  for (const auto& v : s->evaluation.document.volumes) {
    if (v.generated && v.name == "Cell 1") cell = v.id;
  }
  REQUIRE_FALSE(cell.is_null());
  CHECK(w.is_generated(cell));
  CHECK_FALSE(w.edit_in_context(cell));
  // Their composite's generator opens in the Graph dock.
  CHECK(w.edit_graph(r.rack));
  w.leave_graph();

  // Closing never waits on a question (H7: unsaved changes are asked about).
  w.set_unsaved_prompt([] { return app::MainWindow::UnsavedChoice::Discard; });
  w.close();
  std::error_code ignored;
  std::filesystem::remove_all(dir, ignored);
}

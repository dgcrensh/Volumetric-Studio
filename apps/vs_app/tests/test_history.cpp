// Undo and redo over the document (M14 acceptance: "Undo/redo restores
// identical document hashes").

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "authoring.hpp"
#include "history.hpp"
#include "samples.hpp"
#include "vs/model/project.hpp"

using namespace vs;
using core::Id;
using app::History;

namespace {

core::Hash256 hash(const History& h) { return model::content_hash(h.project()); }

const feature::Feature& feature_named(const model::Document& doc, const std::string& name) {
  for (const auto& f : doc.layout.features()) {
    if (doc.layout.label(f.id) == name) return f;
  }
  FAIL("no feature " << name);
  return doc.layout.features().front();
}

}  // namespace

TEST_CASE("A command is applied, undone and redone", "[app][history]") {
  History h(samples::load("bracket.vsproj"));
  const auto h0 = hash(h);
  CHECK_FALSE(h.can_undo());
  CHECK_FALSE(h.dirty());

  const auto d = h.apply(app::rename_document("Bracket, edited"));
  INFO(samples::codes(d));
  REQUIRE_FALSE(d.has_errors());
  CHECK(h.project().document.name == "Bracket, edited");
  CHECK(h.undo_label() == "Rename project");
  CHECK(h.dirty());
  const auto h1 = hash(h);
  CHECK(h1 != h0);

  REQUIRE(h.undo());
  CHECK(hash(h) == h0);
  CHECK_FALSE(h.dirty());
  CHECK(h.redo_label() == "Rename project");
  REQUIRE(h.redo());
  CHECK(hash(h) == h1);
  CHECK_FALSE(h.redo());

  h.mark_saved();
  CHECK_FALSE(h.dirty());
  h.reset(samples::load("bracket.vsproj"));
  CHECK_FALSE(h.can_undo());
  CHECK(hash(h) == h0);
}

TEST_CASE("A change that breaks the document is refused and leaves it as it was", "[app][history]") {
  History h(samples::load("bracket.vsproj"));
  const auto h0 = hash(h);
  // A second volume called "Base".
  const auto d = h.apply(app::add_volume(app::volume_from_features("Base", {})));
  CHECK(d.has_errors());
  CHECK_FALSE(d.with_code("model.volume.duplicate_name").empty());
  CHECK(hash(h) == h0);
  CHECK_FALSE(h.can_undo());

  // A variable loop is refused by the table itself.
  const auto& vars = h.project().document.layout.variables();
  const auto* width = vars.find("width");
  REQUIRE(width != nullptr);
  const auto loop = h.apply(app::set_variable({}, width->id, "width", "depth + width"));
  CHECK(loop.has_errors());
  CHECK(hash(h) == h0);

  // A preview computes the change without keeping it.
  core::Diagnostics why;
  const auto preview = h.try_apply(app::rename_document("Preview"), &why);
  REQUIRE(preview);
  CHECK(preview->document.name == "Preview");
  CHECK(hash(h) == h0);
}

TEST_CASE("Undoing every step of a session restores each document hash", "[app][history][acceptance]") {
  History h(samples::load("bracket.vsproj"));
  std::vector<core::Hash256> hashes{hash(h)};
  const model::Document& doc0 = h.project().document;
  const Id base_block = feature_named(doc0, "Base block").id;
  const Id upright_block = feature_named(doc0, "Upright block").id;
  const Id top = feature_named(doc0, "Top of base").id;
  const Id base = doc0.volumes.at(0).id;
  const Id width = doc0.layout.variables().find("width")->id;

  auto step = [&](const app::Command& c) {
    INFO(c.label);
    const auto d = h.apply(c);
    INFO(samples::codes(d));
    REQUIRE_FALSE(d.has_errors());
    hashes.push_back(hash(h));
    CHECK(hashes.back() != hashes[hashes.size() - 2]);
  };

  step(app::set_variable({}, width, "width", "72 mm"));
  step(app::add_variable({}, "fillet_r", "2 mm"));
  step(app::rename_feature({}, top, "Base top"));
  {
    feature::Feature f = *h.project().document.layout.find(upright_block);
    f.as<feature::ExtrudeFeature>()->depth = feature::Param::of(55.0);
    step(app::replace_feature({}, f));
  }
  step(app::set_suppressed({}, upright_block, true));
  step(app::set_suppressed({}, upright_block, false));
  {
    feature::Feature plane;
    plane.definition = feature::DatumPlaneFeature{feature::PlaneOffset{feature::PrincipalPlane::ZX,
                                                                       feature::Param::of(5.0)}};
    step(app::add_feature({}, plane));
  }
  step(app::move_feature({}, h.project().document.layout.features().back().id, 0));
  {
    // The base volume's contents: a fillet on the fill's top edges.
    const auto& tree = *h.project().document.volumes.at(0).tree();
    const Id fill = tree.features().back().id;
    feature::Feature fillet;
    fillet.definition = feature::FilletFeature{{feature::EdgeRef{feature::FaceRef{fill, "end"}, std::nullopt}},
                                               feature::Param::expr("thickness / 5")};
    step(app::add_feature(base, fillet));
  }
  {
    model::Volume v = *h.project().document.find_volume(base);
    v.seed = 7;
    v.parameters.push_back({Id::random(), "rib", feature::Param::of(1.5)});
    step(app::update_volume(v));
  }
  {
    model::ConnectionSurface s = h.project().document.connections.at(0);
    s.name = "Bolted joint";
    step(app::update_connection(s));
  }
  step(app::add_volume(app::volume_from_features("Extra", {base_block})));
  step(app::remove_volume(h.project().document.volumes.back().id));
  step(app::rename_document("Edited bracket"));

  // Undo everything, checking each state on the way back.
  for (std::size_t i = hashes.size() - 1; i > 0; --i) {
    REQUIRE(h.undo());
    CHECK(hash(h) == hashes[i - 1]);
  }
  CHECK_FALSE(h.can_undo());
  // And redo it all.
  for (std::size_t i = 1; i < hashes.size(); ++i) {
    REQUIRE(h.redo());
    CHECK(hash(h) == hashes[i]);
  }
  CHECK_FALSE(h.can_redo());
}

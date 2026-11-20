#pragma once

// What the panels show, as plain data (no Qt): the selection model, the
// volume tree's rows, the feature tree's rows, the inspector's properties
// and where a diagnostic sends the user. The widgets in main_window.cpp
// only lay these out, so the rules live here and are tested headless.

#include <string>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/core/id.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/build.hpp"
#include "vs/model/document.hpp"
#include "vs/validate/registry.hpp"

namespace vs::app {

using core::Id;

// One selected thing, shared by the volume tree, the feature tree, the
// viewport and the inspector.
struct Selection {
  enum class Kind { None, Volume, ConnectionSurface, Feature, Node };
  Kind kind = Kind::None;
  Id id;      // the volume, surface, feature or node
  Id volume;  // Feature, Node: the volume whose contents hold it (null: the layout)

  static Selection none() { return {}; }
  static Selection of_volume(const Id& v) { return {Kind::Volume, v, v}; }
  static Selection of_surface(const Id& s) { return {Kind::ConnectionSurface, s, {}}; }
  static Selection of_feature(const Id& f, const Id& volume = {}) { return {Kind::Feature, f, volume}; }
  static Selection of_node(const Id& n, const Id& volume) { return {Kind::Node, n, volume}; }

  [[nodiscard]] bool empty() const { return kind == Kind::None; }
  friend bool operator==(const Selection&, const Selection&) = default;
};

// The viewport subject (a volume or a connection surface) a selection
// highlights: a volume, a surface, or the volume whose contents hold a
// feature or node. Null for nothing, or a layout feature.
Id viewport_subject(const Selection& selection);

// Where clicking a diagnostic goes: its "act here" target
// (validate::route()) as a selection. Layout targets naming a volume, its
// boundary or one of its parameters select the volume; other layout
// targets (variables, keep-outs, links, the layout as a whole) select
// nothing.
Selection selection_for(const core::Diagnostic& diagnostic, const model::Document& document);

// --- volume tree ----------------------------------------------------------------------

struct VolumeRow {
  Id id;
  std::string name;
  model::VolumeKind kind = model::VolumeKind::Static;
  bool keep_out = false;
  bool placeholder = false;  // no contents yet
  bool generated = false;    // L4: made by its composite's procedure; read-only
  model::VolumeStatus status = model::VolumeStatus::Stale;
  bool stale_contents = false;
  std::size_t errors = 0, warnings = 0;  // this volume's own diagnostics
  // L3: its composite parent (null at the root) and how deep it sits. Rows
  // come parents first, each followed by its children (a depth-first walk).
  Id parent;
  std::size_t depth = 0;
};

struct SurfaceRow {
  Id id;
  std::string name;
  model::ConnectionType type = model::ConnectionType::Bonded;
  std::vector<std::string> volumes;  // names of the volumes it joins
  bool ok = false;                   // its patch built
};

// `states` overrides the evaluation's statuses (the engine's live states
// while an evaluation runs: Regenerating); empty uses the evaluation's.
std::vector<VolumeRow> volume_rows(const eval::Evaluation& evaluation,
                                   const std::map<Id, model::VolumeStatus>& states = {});
std::vector<SurfaceRow> surface_rows(const eval::Evaluation& evaluation);

// The text for a kind ("Static", "Parametric", "Procedural", "Placeholder", "Keep-out"),
// with "Generated " before it for a generated volume (L4).
std::string kind_label(const VolumeRow& row);
std::string base_kind_label(const VolumeRow& row);  // without "Generated "
std::string status_label(model::VolumeStatus status);

// --- feature tree ---------------------------------------------------------------------

struct FeatureRow {
  Selection selection;       // Feature or Node
  std::string name;
  std::string kind;          // "extrude", "field.shell", ...
  std::string status;        // "ok", "failed", "suppressed", or empty
};

// The layout's features (volume null), a parametric volume's features, or
// a procedural volume's graph nodes. Static volumes have none.
std::vector<FeatureRow> feature_rows(const eval::Evaluation& evaluation, const Id& volume);

// --- inspector ------------------------------------------------------------------------

struct Property {
  std::string group;  // "Volume", "Parameters", "Published outputs", ...
  std::string name;
  std::string value;
};

std::vector<Property> inspect(const eval::Evaluation& evaluation, const Selection& selection);

}  // namespace vs::app

#pragma once

// Configurations and design tables in the app (L5, decision 0037): the
// commands that edit them and what the toolbar, the design table dialog
// and the inspector show. Plain data, no Qt.
//
// Every change is one undoable Command (history.hpp). History drops values
// whose target an edit removed (model::prune_configurations()), so
// deleting a variable, parameter or volume is never refused because a
// configuration drives it.

#include <optional>
#include <string>
#include <vector>

#include "history.hpp"
#include "vs/model/configuration.hpp"
#include "vs/model/design_table.hpp"
#include "vs/model/document.hpp"

namespace vs::app {

using core::Id;

// --- commands ---------------------------------------------------------------------------------

// Adds a configuration named `name` (made unique), with the values of
// `copy_of` (null: none), and makes it active when `activate`. Its id is
// `id` (null: random), so a caller can refer to it afterwards.
Command add_configuration(std::string name, Id copy_of = {}, bool activate = true, Id id = {});
Command rename_configuration(Id configuration, std::string name);
// Removes it; the authored values show when it was active.
Command remove_configuration(Id configuration);
// Sets the value `configuration` gives `target` (empty: it no longer
// drives it).
Command set_configuration_value(Id configuration, Id target, std::string expression);
// Shows a configuration (null: the authored values).
Command set_active_configuration(Id configuration);
// Makes the table the document's configurations (model::apply_design_table()).
Command set_design_table(model::DesignTable table);

// --- what the app shows -----------------------------------------------------------------------

// The toolbar's choices: the authored values first (null id), then each
// configuration in document order.
struct ConfigurationChoice {
  Id id;
  std::string label;
  bool active = false;
};
std::vector<ConfigurationChoice> configuration_choices(const model::Document& document);

// The value the active configuration gives `target`, if it drives it (the
// inspector shows it beside the authored one).
std::optional<std::string> active_value(const model::Document& document, const Id& target);

// The design table dialog's grid: every configuration as a row, and the
// driven numbers as columns, plus `extra` columns the user added (targets
// no configuration drives yet), in drivable order.
model::DesignTable table_for_editing(const model::Document& document,
                                     const std::vector<Id>& extra = {});

// The drivable numbers that aren't columns of `table` yet: what the
// dialog's "Add column" offers.
std::vector<model::Drivable> addable_columns(const model::Document& document,
                                             const model::DesignTable& table);

}  // namespace vs::app

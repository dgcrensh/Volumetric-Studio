#pragma once

// Configurations (L5, decision 0037): named variants of one document.
//
// A Configuration (document.hpp) gives values to some of the numbers a
// volume's fixed inputs are made of: what a link may drive, at any depth.
//
//   a variable of the root layout          `width`
//   a variable of a composite's layout     `Housing/width`
//   a volume's parameter                   `Base.thickness`
//
// (the labels are the design table's column names, design_table.hpp).
// Nothing inside a volume's contents can be driven (design rule 1): a
// parametric volume's own numbers follow its parameters, a procedure's
// are its volume's parameters by name.
//
// A value is an expression evaluated where its target is: a variable's in
// its own table (so it may use the other variables there), a parameter's
// in its volume's level, as the parameter's authored expression would be.
// A parameter given a plain number takes it as its value, so a
// configuration that repeats the authored number builds exactly the
// authored document.
//
// The document builds its active configuration: configured() is what
// model::build(), vs_validate and vs_eval build. Cache keys see values
// only, never which configuration produced them, so configurations that
// agree on a volume's inputs share its cached contents.
//
// check() (check.hpp) refuses a configuration that wouldn't build:
//   model.configuration.name        no name, or another configuration's
//   model.configuration.target      a value's target can't be driven (or is
//                                   driven twice by one configuration)
//   model.configuration.expression  a value doesn't parse, names something
//                                   its target can't see, or closes a
//                                   variable loop
//   model.configuration.loop        it closes a dependency loop through links
//   model.configuration.active      the active configuration doesn't exist
//   model.configuration.linked      (warning) a link drives the same target;
//                                   the link's value wins once it has one

#include <optional>
#include <string>
#include <vector>

#include "vs/model/document.hpp"

namespace vs::model {

enum class DrivableKind {
  Variable,           // of the root layout
  CompositeVariable,  // of a composite's layout; owner: the composite
  Parameter,          // owner: the volume
};

// A number a configuration may drive.
struct Drivable {
  DrivableKind kind = DrivableKind::Variable;
  Id target;
  Id owner;                // the volume, except for root variables
  std::string label;       // `width`, `Housing/width`, `Base.thickness`
  std::string expression;  // as authored: a variable's expression, a
                           // parameter's expression or its number
  friend bool operator==(const Drivable&, const Drivable&) = default;
};

// What `target` names, if a configuration may drive it. Generated volumes
// (L4) are never drivable: they exist only in evaluated documents.
std::optional<Drivable> drivable(const Document& document, const Id& target);

// Every drivable number in document order: the root's variables (in table
// order), then each authored volume's parameters and, for a composite, its
// layout's variables.
std::vector<Drivable> drivable_targets(const Document& document);

// The document with `configuration`'s values written into their targets.
// The result has no configurations and no active one: it is a document as
// it builds. An unknown id applies nothing.
Document apply_configuration(const Document& document, const Id& configuration);

// The document as it builds: its active configuration applied, or (with
// none active) the document itself, also without configurations.
Document configured(const Document& document);

// Whether `document` would be changed by configured(): a configuration is
// active, or there are configurations to strip.
bool has_configurations(const Document& document);

// Drops every configuration value whose target is no longer drivable (a
// removed variable, parameter or volume), as removing a volume drops its
// links. Returns how many it dropped.
std::size_t prune_configurations(Document& document);

// The text a value is written as: a number in the shortest form that reads
// back as the same double ("12", "0.1", "1e-07").
std::string number_text(double value);

// `text` as a plain number (surrounding spaces allowed), or nullopt for an
// expression.
std::optional<double> plain_number(std::string_view text);

}  // namespace vs::model

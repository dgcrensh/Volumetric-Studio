#pragma once

// Habit guidance (M16, spec: CAD habits get redirected rather than
// blocked). Plain data and rules, no Qt.
//
// A user used to feature-based CAD will select a generated face and reach
// for fillet, cut or mate. In a volumetric design those act elsewhere:
//
//   fillet  a generated surface has no edges to round; its shape comes from
//           the procedure, so edit the procedure's parameters (or, for
//           modelled contents, fillet inside the volume, in context);
//   cut     never carve generated contents: mark where material must not be
//           with a keep-out region, and the contents regenerate around it;
//   mate    generated faces move whenever the contents regenerate; mate to
//           the volume's connection surface, which stays where the layout
//           puts it.
//
// guide() says which, for the volume and the habit, with the one action
// that does the right thing; the main window shows it inline, with a button.

#include <optional>
#include <string>

#include "vs/core/id.hpp"
#include "vs/model/document.hpp"

namespace vs::app {

enum class Habit { Fillet, Cut, Mate };
std::string_view to_string(Habit habit);

struct Guidance {
  enum class Action {
    EditGraph,         // open the procedure in the node editor
    Unbake,            // a baked volume: back to its procedure first
    EditInContext,     // modelled contents: edit the volume's own features
    AddKeepOut,        // a keep-out region
    UseConnection,     // select the connection surface that joins the volume
    AddConnection,     // none joins it yet: make one
  };
  Habit habit = Habit::Fillet;
  core::Id volume;
  std::string title;   // one line: "Generated contents can't be filleted"
  std::string text;    // the explanation, a few sentences
  Action action = Action::EditGraph;
  std::string action_label;  // the button: "Edit the procedure"
  core::Id target;     // UseConnection: the surface; otherwise the volume
};

std::string_view to_string(Guidance::Action action);

// Guidance for `habit` on `volume`'s contents; nullopt when the habit is
// fine as it is (a fillet inside modelled contents being edited in context)
// or the volume doesn't exist.
std::optional<Guidance> guide(const model::Document& document, const core::Id& volume, Habit habit,
                              const core::Id& context_volume = {});

}  // namespace vs::app

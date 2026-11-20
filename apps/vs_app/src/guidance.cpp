#include "guidance.hpp"

#include <fmt/format.h>

#include "vs/proc/bake.hpp"

namespace vs::app {

std::string_view to_string(Habit habit) {
  switch (habit) {
    case Habit::Fillet: return "fillet";
    case Habit::Cut: return "cut";
    case Habit::Mate: return "mate";
  }
  return "?";
}

std::string_view to_string(Guidance::Action action) {
  switch (action) {
    case Guidance::Action::EditGraph: return "edit graph";
    case Guidance::Action::Unbake: return "unbake";
    case Guidance::Action::EditInContext: return "edit in context";
    case Guidance::Action::AddKeepOut: return "add keep-out";
    case Guidance::Action::UseConnection: return "use connection surface";
    case Guidance::Action::AddConnection: return "add connection surface";
  }
  return "?";
}

std::optional<Guidance> guide(const model::Document& document, const core::Id& volume, Habit habit,
                              const core::Id& context_volume) {
  const model::Volume* v = document.find_volume(volume);
  if (v == nullptr) return std::nullopt;
  Guidance g;
  g.habit = habit;
  g.volume = volume;
  g.target = volume;
  const std::string& name = v->name;
  const bool baked = proc::is_baked(*v);
  const bool procedural = v->kind() == model::VolumeKind::Procedural;
  const bool parametric = v->tree() != nullptr;
  const bool placeholder = v->contents_empty() && !parametric;
  const bool mesh_static = !procedural && !parametric && !placeholder;  // imported or baked

  switch (habit) {
    case Habit::Fillet:
      if (parametric) {
        // Modelled contents have real edges: fillet them inside the volume.
        if (context_volume == volume) return std::nullopt;
        g.title = fmt::format("Fillet {}'s own edges, inside the volume", name);
        g.text = fmt::format(
            "{}'s contents are modelled, so their edges can be filleted, but from inside the volume: edit its "
            "contents in context, where only its boundary and connection surfaces can be referenced.",
            name);
        g.action = Guidance::Action::EditInContext;
        g.action_label = "Edit contents in context";
        return g;
      }
      if (procedural) {
        g.title = "Generated surfaces have no edges to fillet";
        g.text = fmt::format(
            "{}'s surface comes from its procedure and is made again on every change, so a fillet on it "
            "would be lost. Round it where it is made: edit the procedure's parameters (a smooth union's "
            "blend, a thickness, a cell size).",
            name);
        g.action = Guidance::Action::EditGraph;
        g.action_label = "Edit the procedure";
        return g;
      }
      if (baked) {
        g.title = "Baked contents are a fixed mesh";
        g.text = fmt::format(
            "{} was baked from a procedure: its mesh has no edges to fillet. Unbake it to get the procedure "
            "back, and round the shape there.",
            name);
        g.action = Guidance::Action::Unbake;
        g.action_label = "Unbake";
        return g;
      }
      if (mesh_static) {
        g.title = "Imported contents can't be filleted here";
        g.text = fmt::format(
            "{}'s contents are an imported shape, kept as it is. Fillet it in the tool it came from and import "
            "it again, or clear it and fill the volume by modelling.",
            name);
        g.action = Guidance::Action::EditInContext;
        g.action_label = "Fill by modelling instead";
        return g;
      }
      g.title = fmt::format("{} has nothing to fillet yet", name);
      g.text = "A placeholder is only its boundary. Fill it by modelling to make features that can be filleted.";
      g.action = Guidance::Action::EditInContext;
      g.action_label = "Fill by modelling";
      return g;

    case Habit::Cut:
      g.title = parametric ? "Cut with a keep-out, not into the contents" : "Generated contents aren't cut";
      g.text = fmt::format(
          "Where material must not be is a keep-out region: {} (and every volume it meets) regenerates around "
          "it, and stays clear of it however its contents change. A cut made into {} contents would be lost "
          "or out of date on the next change.",
          name, procedural ? "procedural" : baked ? "baked" : parametric ? "modelled" : "its");
      g.action = Guidance::Action::AddKeepOut;
      g.action_label = "Add a keep-out region";
      return g;

    case Habit::Mate: {
      g.title = "Mate to the connection surface";
      g.text = fmt::format(
          "Faces of {} move whenever its contents regenerate, so a mate to one would break. Its connection "
          "surfaces stay where the layout puts them: mate to one of those.",
          name);
      for (const auto& s : document.connections) {
        if (s.joins(volume)) {
          g.action = Guidance::Action::UseConnection;
          g.action_label = fmt::format("Select {}", s.name);
          g.target = s.id;
          return g;
        }
      }
      g.text += " None joins it yet.";
      g.action = Guidance::Action::AddConnection;
      g.action_label = "Add a connection surface";
      return g;
    }
  }
  return std::nullopt;
}

}  // namespace vs::app

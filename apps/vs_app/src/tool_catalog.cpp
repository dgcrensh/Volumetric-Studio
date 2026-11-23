#include "tool_catalog.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include <fmt/format.h>

#include "authoring.hpp"

namespace vs::app {

namespace {

using T = ToolId;

constexpr std::array kTools = {
    // Layout: create
    Tool{T::NewVolume, "new_volume", "Volume", "New volume: a box between two layout points, or a layout solid", "",
         Icon::Volume},
    Tool{T::NewConnection, "new_connection", "Surface", "New connection surface where two volumes meet", "",
         Icon::Surface},
    Tool{T::NewKeepOut, "new_keep_out", "Keep-out", "New keep-out region: space contents must leave empty", "",
         Icon::KeepOut},
    Tool{T::NewLink, "new_link", "Link", "Drive a layout number from a volume's published output", "", Icon::Link},
    // Geometry of the tree being edited
    Tool{T::Sketch, "sketch", "Sketch", "New sketch on a plane", "Ctrl+Shift+K", Icon::Sketch},
    Tool{T::DatumPlane, "datum_plane", "Plane", "Datum plane", "", Icon::Plane},
    Tool{T::DatumAxis, "datum_axis", "Axis", "Datum axis", "", Icon::Axis},
    Tool{T::DatumPoint, "datum_point", "Point", "Datum point", "", Icon::Point},
    Tool{T::Extrude, "extrude", "Extrude", "Extrude a sketch's profile", "Ctrl+Shift+E", Icon::Extrude},
    Tool{T::Revolve, "revolve", "Revolve", "Revolve a sketch's profile about an axis", "", Icon::Revolve},
    Tool{T::Sweep, "sweep", "Sweep", "Sweep a profile along a path sketch", "", Icon::Sweep},
    Tool{T::Loft, "loft", "Loft", "Loft through section sketches", "", Icon::Loft},
    Tool{T::Shell, "shell", "Shell", "Hollow the body, leaving walls", "", Icon::Shell},
    Tool{T::Fillet, "fillet", "Fillet", "Round edges", "", Icon::Fillet},
    Tool{T::Chamfer, "chamfer", "Chamfer", "Bevel edges", "", Icon::Chamfer},
    Tool{T::LinearPattern, "linear_pattern", "Linear", "Repeat features along a direction", "", Icon::LinearPattern},
    Tool{T::CircularPattern, "circular_pattern", "Circular", "Repeat features round an axis", "",
         Icon::CircularPattern},
    // Layout: fill the selected volume
    Tool{T::EditContents, "edit_contents", "Model", "Model inside the selected volume (edit its contents in context)",
         "Ctrl+E", Icon::Model},
    Tool{T::EditGraph, "edit_graph", "Procedure", "Fill the selected volume with a procedure, or edit its graph",
         "Ctrl+G", Icon::Procedure},
    Tool{T::AttachTemplate, "attach_template", "Template", "Fill with a ready-made procedure: infill, lattice, bosses",
         "", Icon::Template},
    Tool{T::ImportContents, "import_contents", "Import", "Import STEP, IGES, STL, 3MF or OBJ into the selected volume",
         "", Icon::Import},
    // Layout: structure
    Tool{T::MakeComposite, "make_composite", "Composite", "Make the selected volume a composite that holds children",
         "", Icon::Composite},
    Tool{T::GenerateChildren, "generate_children", "Generate", "Generate the composite's children by procedure", "",
         Icon::Generate},
    // Layout: check
    Tool{T::Validate, "validate", "Validate", "Check the whole design before shipping it", "Ctrl+R", Icon::Validate},
    // Contents
    Tool{T::LeaveContext, "leave_context", "Done", "Done editing contents: back to the layout", "", Icon::Done},
    // Sketch: draw
    Tool{T::SketchSelect, "sketch_select", "Select", "Select sketch geometry and constraints (Esc)", "", Icon::Select},
    Tool{T::SketchPoint, "sketch_point", "Point", "Point", "", Icon::Point},
    Tool{T::SketchLine, "sketch_line", "Line", "Lines, chained until Esc", "", Icon::Line},
    Tool{T::SketchRectangle, "sketch_rectangle", "Rectangle", "Rectangle from two corners", "", Icon::Rectangle},
    Tool{T::SketchCircle, "sketch_circle", "Circle", "Circle from its centre and a point on it", "", Icon::Circle},
    Tool{T::SketchArc, "sketch_arc", "Arc", "Arc from its centre, start and end", "", Icon::Arc},
    Tool{T::SketchSlot, "sketch_slot", "Slot", "Slot from two centres and a width", "", Icon::Slot},
    // Sketch: constrain
    Tool{T::Coincident, "coincident", "Coincident", "Join two points", "", Icon::Coincident},
    Tool{T::PointOnCurve, "point_on_curve", "On curve", "Keep a point on a line or circle", "", Icon::PointOnCurve},
    Tool{T::Horizontal, "horizontal", "Horizontal", "Make a line, or two points, horizontal", "", Icon::Horizontal},
    Tool{T::Vertical, "vertical", "Vertical", "Make a line, or two points, vertical", "", Icon::Vertical},
    Tool{T::Parallel, "parallel", "Parallel", "Make two lines parallel", "", Icon::Parallel},
    Tool{T::Perpendicular, "perpendicular", "Perpendicular", "Make two lines perpendicular", "", Icon::Perpendicular},
    Tool{T::Tangent, "tangent", "Tangent", "Make a line and a circle, or two circles, tangent", "", Icon::Tangent},
    Tool{T::Equal, "equal", "Equal", "Equal lengths, or equal radii", "", Icon::Equal},
    Tool{T::Symmetric, "symmetric", "Symmetric", "Mirror two points about a line or a point", "", Icon::Symmetric},
    Tool{T::Fix, "fix", "Fix", "Keep geometry where it is now", "", Icon::Fix},
    // Sketch: dimension
    Tool{T::Distance, "distance", "Distance", "Distance between points, a point and a line, or a line's length", "",
         Icon::Distance},
    Tool{T::HorizontalDistance, "horizontal_distance", "H distance", "Horizontal distance between two points", "",
         Icon::HorizontalDistance},
    Tool{T::VerticalDistance, "vertical_distance", "V distance", "Vertical distance between two points", "",
         Icon::VerticalDistance},
    Tool{T::Angle, "angle", "Angle", "Angle of a line, or between two lines", "", Icon::Angle},
    Tool{T::Radius, "radius", "Radius", "Radius of a circle or arc", "", Icon::Radius},
    Tool{T::Diameter, "diameter", "Diameter", "Diameter of a circle or arc", "", Icon::Diameter},
    // Sketch: edit and finish
    Tool{T::Construction, "construction", "Construction", "Toggle construction geometry (not part of profiles)", "",
         Icon::Construction},
    Tool{T::DeleteSketchItems, "delete_sketch_items", "Delete", "Delete the selected geometry and constraints (Del)",
         "", Icon::Delete},
    Tool{T::CancelSketch, "cancel_sketch", "Cancel", "Leave the sketch without keeping changes", "", Icon::Cancel},
    Tool{T::FinishSketch, "finish_sketch", "Finish", "Keep the sketch and go back", "", Icon::Done},
    // Graph
    Tool{T::Bake, "bake", "Bake", "Store the generated mesh and stop regenerating it", "", Icon::Bake},
    Tool{T::Unbake, "unbake", "Unbake", "Back to the procedure the mesh was baked from", "", Icon::Unbake},
    Tool{T::LeaveGraph, "leave_graph", "Done", "Done editing the procedure", "", Icon::Done},
    // Drawing
    Tool{T::NewDrawing, "new_drawing", "New", "A standard drawing of what is shown: four views, overall dimensions",
         "", Icon::Drawing},
    Tool{T::AddDrawingView, "add_drawing_view", "View", "Add a view to the drawing", "", Icon::View},
    Tool{T::AddDrawingDimension, "add_drawing_dimension", "Dimension", "Add a dimension to a view", "",
         Icon::HorizontalDistance},
    Tool{T::ExportDrawing, "export_drawing", "Export", "Export the drawing as SVG, PDF or DXF", "", Icon::Export},
    Tool{T::CloseDrawing, "close_drawing", "Close", "Close the drawing: back to the layout", "", Icon::Close},
};

constexpr std::size_t kAll = std::numeric_limits<std::size_t>::max();

std::vector<ToolGroup> make_groups(Context context) {
  switch (context) {
    case Context::Layout:
      return {
          {"Create", {T::NewVolume, T::NewConnection, T::NewKeepOut, T::NewLink}, kAll},
          {"Layout geometry",
           {T::Sketch, T::DatumPlane, T::DatumPoint, T::Extrude, T::DatumAxis, T::Revolve, T::Sweep, T::Loft,
            T::Shell, T::Fillet, T::Chamfer, T::LinearPattern, T::CircularPattern},
           4},
          {"Fill selected", {T::EditContents, T::EditGraph, T::AttachTemplate, T::ImportContents, T::Bake, T::Unbake},
           4},
          {"Structure", {T::MakeComposite, T::GenerateChildren}, kAll},
          {"Check", {T::Validate}, kAll},
      };
    case Context::Contents:
      return {
          {"Sketch", {T::Sketch}, kAll},
          {"Reference", {T::DatumPlane, T::DatumAxis, T::DatumPoint}, kAll},
          {"Solids", {T::Extrude, T::Revolve, T::Sweep, T::Loft}, kAll},
          {"Modify", {T::Shell, T::Fillet, T::Chamfer}, kAll},
          {"Pattern", {T::LinearPattern, T::CircularPattern}, kAll},
          {"Finish", {T::LeaveContext}, kAll},
      };
    case Context::Sketch:
      return {
          {"Draw", {T::SketchSelect, T::SketchLine, T::SketchRectangle, T::SketchCircle, T::SketchArc, T::SketchPoint,
                    T::SketchSlot}, 5},
          {"Constrain", {T::Coincident, T::PointOnCurve, T::Horizontal, T::Vertical, T::Parallel, T::Perpendicular,
                         T::Tangent, T::Equal, T::Symmetric, T::Fix}, 4},
          {"Dimension", {T::Distance, T::HorizontalDistance, T::VerticalDistance, T::Angle, T::Radius, T::Diameter},
           2},
          {"Edit", {T::Construction, T::DeleteSketchItems}, kAll},
          {"Finish", {T::CancelSketch, T::FinishSketch}, kAll},
      };
    case Context::Graph:
      return {
          {"Procedure", {T::AttachTemplate, T::Bake}, kAll},
          {"Finish", {T::LeaveGraph}, kAll},
      };
    case Context::Drawing:
      return {
          {"Drawing", {T::NewDrawing, T::AddDrawingView, T::AddDrawingDimension}, kAll},
          {"Share", {T::ExportDrawing}, kAll},
          {"Finish", {T::CloseDrawing}, kAll},
      };
  }
  return {};
}

Availability ok() { return {}; }
Availability no(std::string reason) { return {false, std::move(reason)}; }

std::string quoted(const model::Volume& v) { return fmt::format("'{}'", v.name); }

// What a fill tool can do to `v`, as the main window's entry points allow.
Availability fill(ToolId id, const model::Volume* v, bool generated) {
  if (v == nullptr) return no("Select a volume first.");
  if (generated) {
    return no(fmt::format("{} is made by its composite's procedure; edit that procedure instead.", quoted(*v)));
  }
  if (v->keep_out) return no(fmt::format("{} is a keep-out: it has no contents.", quoted(*v)));
  const auto kind = contents_kind(*v);
  const bool placeholder = kind == ContentsKind::Placeholder;
  switch (id) {
    case T::EditContents:
      if (placeholder || kind == ContentsKind::Parametric || kind == ContentsKind::Composite) return ok();
      return no(fmt::format("{} isn't modelled; swap its contents to modelled first.", quoted(*v)));
    case T::EditGraph:
      if (placeholder || kind == ContentsKind::Procedural || kind == ContentsKind::Composite) return ok();
      if (kind == ContentsKind::Baked) return no(fmt::format("Unbake {} to edit its procedure.", quoted(*v)));
      return no(fmt::format("{} isn't procedural; swap its contents to a procedure first.", quoted(*v)));
    case T::AttachTemplate:
      if (placeholder || kind == ContentsKind::Procedural) return ok();
      return no(fmt::format("Templates fill an empty or procedural volume; {} isn't one.", quoted(*v)));
    case T::ImportContents:
      if (kind == ContentsKind::Composite) return no(fmt::format("{} is a composite: import into a child.", quoted(*v)));
      return ok();
    case T::Bake:
      if (kind == ContentsKind::Procedural) return ok();
      return no(fmt::format("Only a procedural volume can be baked; {} isn't one.", quoted(*v)));
    case T::Unbake:
      if (kind == ContentsKind::Baked) return ok();
      return no(fmt::format("{} isn't baked.", quoted(*v)));
    case T::MakeComposite:
      if (kind == ContentsKind::Composite) return no(fmt::format("{} is already a composite.", quoted(*v)));
      return ok();
    case T::GenerateChildren:
      if (kind == ContentsKind::Composite) return ok();
      return no(fmt::format("Make {} a composite first.", quoted(*v)));
    default: return ok();
  }
}

}  // namespace

std::string_view to_string(Context context) {
  switch (context) {
    case Context::Layout: return "layout";
    case Context::Contents: return "contents";
    case Context::Sketch: return "sketch";
    case Context::Graph: return "graph";
    case Context::Drawing: return "drawing";
  }
  return "unknown";
}

std::string_view title(Context context) {
  switch (context) {
    case Context::Layout: return "Layout";
    case Context::Contents: return "Contents";
    case Context::Sketch: return "Sketch";
    case Context::Graph: return "Graph";
    case Context::Drawing: return "Drawing";
  }
  return "";
}

std::span<const Tool> all_tools() { return kTools; }

const Tool& tool(ToolId id) {
  const auto i = static_cast<std::size_t>(id);
  return i < kTools.size() ? kTools.at(i) : kTools.front();
}

std::optional<ToolId> find_tool(std::string_view name) {
  for (const Tool& t : kTools) {
    if (t.name == name) return t.id;
  }
  return std::nullopt;
}

const std::vector<ToolGroup>& tool_groups(Context context) {
  static const auto table = [] {
    std::array<std::vector<ToolGroup>, std::size(kContexts)> t;
    for (Context c : kContexts) t.at(static_cast<std::size_t>(c)) = make_groups(c);
    return t;
  }();
  return table.at(static_cast<std::size_t>(context));
}

bool shows_in(ToolId id, Context context) {
  for (const ToolGroup& g : tool_groups(context)) {
    if (std::find(g.tools.begin(), g.tools.end(), id) != g.tools.end()) return true;
  }
  return false;
}

std::optional<feature::FeatureKind> feature_kind(ToolId id) {
  using K = feature::FeatureKind;
  switch (id) {
    case T::Sketch: return K::Sketch;
    case T::DatumPlane: return K::DatumPlane;
    case T::DatumAxis: return K::DatumAxis;
    case T::DatumPoint: return K::DatumPoint;
    case T::Extrude: return K::Extrude;
    case T::Revolve: return K::Revolve;
    case T::Sweep: return K::Sweep;
    case T::Loft: return K::Loft;
    case T::Shell: return K::Shell;
    case T::Fillet: return K::Fillet;
    case T::Chamfer: return K::Chamfer;
    case T::LinearPattern: return K::LinearPattern;
    case T::CircularPattern: return K::CircularPattern;
    default: return std::nullopt;
  }
}

std::optional<sketch::ConstraintKind> constraint_kind(ToolId id) {
  using K = sketch::ConstraintKind;
  switch (id) {
    case T::Coincident: return K::Coincident;
    case T::PointOnCurve: return K::PointOnCurve;
    case T::Horizontal: return K::Horizontal;
    case T::Vertical: return K::Vertical;
    case T::Parallel: return K::Parallel;
    case T::Perpendicular: return K::Perpendicular;
    case T::Tangent: return K::Tangent;
    case T::Equal: return K::Equal;
    case T::Symmetric: return K::Symmetric;
    case T::Fix: return K::Fix;
    case T::Distance: return K::Distance;
    case T::HorizontalDistance: return K::HorizontalDistance;
    case T::VerticalDistance: return K::VerticalDistance;
    case T::Angle: return K::Angle;
    case T::Radius: return K::Radius;
    case T::Diameter: return K::Diameter;
    default: return std::nullopt;
  }
}

core::Id selected_volume(const ToolState& state) {
  return state.selection.kind == Selection::Kind::Volume ? state.selection.id : core::Id{};
}

bool selection_is_sketch(const model::Document& document, const Selection& selection) {
  if (selection.kind != Selection::Kind::Feature) return false;
  const feature::FeatureTree* tree = &document.layout;
  if (!selection.volume.is_null()) {
    const model::Volume* v = document.find_volume(selection.volume);
    tree = v != nullptr ? v->tree() : nullptr;
  }
  const feature::Feature* f = tree != nullptr ? tree->find(selection.id) : nullptr;
  return f != nullptr && f->kind() == feature::FeatureKind::Sketch;
}

Context current_context(const model::Document& document, const ToolState& state) {
  if (state.sketch_open) return Context::Sketch;
  if (state.drawing_shown) return Context::Drawing;
  if (!state.graph_volume.is_null()) return Context::Graph;
  if (!state.editing_volume.is_null()) {
    const model::Volume* v = document.find_volume(state.editing_volume);
    if (v != nullptr && !v->composite()) return Context::Contents;
  }
  return Context::Layout;
}

Availability tab_availability(Context tab, const model::Document& document, const ToolState& state) {
  const Context now = current_context(document, state);
  if (tab == now) return ok();
  if (now == Context::Sketch) return no("Finish or cancel the sketch first.");
  const core::Id vid = selected_volume(state);
  const model::Volume* v = vid.is_null() ? nullptr : document.find_volume(vid);
  switch (tab) {
    case Context::Layout: return ok();
    case Context::Contents: {
      if (v == nullptr) return no("Select a volume to model inside it.");
      if (v->composite()) return no(fmt::format("{} is a composite: model inside one of its children.", quoted(*v)));
      return fill(T::EditContents, v, state.selection_generated);
    }
    case Context::Sketch:
      if (selection_is_sketch(document, state.selection)) return ok();
      return no("Select a sketch to edit it, or add one with the Sketch tool.");
    case Context::Graph:
      if (v == nullptr) return no("Select a volume to fill it with a procedure.");
      return fill(T::EditGraph, v, state.selection_generated);
    case Context::Drawing:
      if (!document.drawings.empty() || state.evaluated) return ok();
      return no("Nothing has been evaluated to draw yet.");
  }
  return ok();
}

Availability availability(ToolId id, const model::Document& document, const ToolState& state) {
  const Context now = current_context(document, state);
  if (!shows_in(id, now)) return no(fmt::format("Not available in {}.", title(now)));
  switch (now) {
    case Context::Layout: {
      switch (id) {
        case T::EditContents:
        case T::EditGraph:
        case T::AttachTemplate:
        case T::ImportContents:
        case T::Bake:
        case T::Unbake:
        case T::MakeComposite:
        case T::GenerateChildren: {
          const core::Id vid = selected_volume(state);
          return fill(id, vid.is_null() ? nullptr : document.find_volume(vid), state.selection_generated);
        }
        default: return ok();
      }
    }
    case Context::Contents: return ok();
    case Context::Sketch: {
      if (const auto kind = constraint_kind(id)) {
        const auto& fits = state.applicable_constraints;
        if (std::find(fits.begin(), fits.end(), *kind) != fits.end()) return ok();
        return no(state.sketch_selection == 0 ? "Select the geometry to constrain first."
                                              : "Doesn't fit the selected geometry.");
      }
      if ((id == T::Construction || id == T::DeleteSketchItems) && state.sketch_selection == 0) {
        return no("Select sketch geometry first.");
      }
      return ok();
    }
    case Context::Graph: {
      if (id == T::LeaveGraph) return ok();
      const model::Volume* v = document.find_volume(state.graph_volume);
      return fill(id, v, false);
    }
    case Context::Drawing:
      if (id == T::NewDrawing && !state.evaluated) return no("Nothing has been evaluated to draw yet.");
      return ok();
  }
  return ok();
}

}  // namespace vs::app

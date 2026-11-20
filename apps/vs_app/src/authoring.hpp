#pragma once

// Every model edit the app makes, as a Command for the History
// (history.hpp). The menus, the feature tree, the dialogs and sketch mode
// build these and nothing else changes a project, so every edit can be
// undone. Plain data, no Qt.
//
// A tree is named by the volume whose contents hold it, null for the
// layout. Commands that can't apply (an unknown id, a variable name in
// use) return errors, which refuse them; History also refuses any change
// that adds an error to model::check().

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "history.hpp"
#include "vs/core/settings.hpp"
#include "vs/feature/tree.hpp"
#include "vs/mesh/mesh.hpp"
#include "vs/model/document.hpp"
#include "vs/proc/graph.hpp"

namespace vs::app {

using core::Id;

// "Extrude", "Datum plane", ...: the label a new feature of `kind` gets in
// menus and commands.
std::string kind_title(feature::FeatureKind kind);

// `base`, or `base 2`, `base 3`, ... whichever isn't in `taken`.
std::string unique_name(const std::string& base, const std::vector<std::string>& taken);

// --- features ---------------------------------------------------------------------------

// Adds `feature` to the tree at `index` (nullopt: the end). A null id gets
// a random one (a sketch: its sketch's id).
Command add_feature(Id volume, feature::Feature feature, std::optional<std::size_t> index = std::nullopt);
// Replaces the feature with the same id (its definition, name and
// suppression; its place stays).
Command replace_feature(Id volume, feature::Feature feature);
Command remove_feature(Id volume, Id feature);
Command move_feature(Id volume, Id feature, std::size_t index);
Command set_suppressed(Id volume, Id feature, bool suppressed);
Command rename_feature(Id volume, Id feature, std::string name);
// Replaces a sketch feature's sketch (sketch mode's result).
Command set_sketch(Id volume, Id feature, sketch::Sketch sketch);

// --- variables (the layout's, or a parametric volume's own) ---------------------------------

Command add_variable(Id volume, std::string name, std::string expression);
Command set_variable(Id volume, Id variable, std::string name, std::string expression);
Command remove_variable(Id volume, Id variable);

// --- volumes ------------------------------------------------------------------------------

// A new volume (placeholder contents) bounded by layout solid features.
// `parent`: the composite it goes in (L3), null at the root; the features or
// points are then of that composite's layout.
model::Volume volume_from_features(std::string name, std::vector<Id> features, Id parent = {});
// A new volume bounded by the box between two layout points.
model::Volume volume_from_box(std::string name, feature::PointRef min, feature::PointRef max, Id parent = {});

Command add_volume(model::Volume volume);
// Replaces the volume with the same id: name, boundary, keep-out flag,
// parameters, seed and outputs (and contents, as given).
Command update_volume(model::Volume volume);
// Removes a volume and what only made sense with it: links from or to it,
// connection surfaces left joining nothing, and its place in keep-outs.
Command remove_volume(Id volume);
Command set_keep_out(Id volume, bool keep_out);
// Empty parametric contents (a tree to fill), or back to a placeholder.
Command make_parametric(Id volume);
Command make_placeholder(Id volume);
// Imports a file as the volume's static contents (vs_eval import.hpp).
Command import_contents(Id volume, std::vector<std::byte> file, std::string format,
                        core::ProductSettings settings);
// Imports a file as a new static volume boxed by its bounds (M17,
// vs_eval import_volume()), named `name`.
Command import_volume(std::vector<std::byte> file, std::string format, std::string name,
                      core::ProductSettings settings);

// --- procedural contents (M15) ---------------------------------------------------------------

// A graph that fills the whole boundary: Boundary -> Output. What "Fill by
// procedure" starts from.
proc::Graph starter_graph();
// Fills a placeholder with `graph` (or replaces a procedural volume's
// graph: attaching a template). Refused for other contents and for graphs
// with errors other than unfinished ones (check_volume_graph(): a node
// reading a surface that doesn't join the volume, say).
Command make_procedural(Id volume, proc::Graph graph, std::string label = "Fill by procedure");
// Replaces a procedural volume's graph, or a composite's generator (L4): the
// node editor's every change.
Command set_graph(Id volume, proc::Graph graph, std::string label);
// A procedural volume's graph (empty for an empty one), or a generating
// composite's generator (L4); nullopt when it
// isn't procedural or its JSON doesn't parse.
std::optional<proc::Graph> graph_of(const model::Document& document, const Id& volume);
// proc::bake() with the volume's generated mesh, and proc::unbake().
Command bake_volume(Id volume, mesh::Mesh mesh);
Command unbake_volume(Id volume);

// --- swapping contents (M16) -------------------------------------------------------------------

// What a volume's contents can be swapped to.
enum class ContentsKind { Placeholder, Parametric, Procedural, Baked, Composite };
std::string_view to_string(ContentsKind kind);
// The kind a volume's contents are now (nullopt: a keep-out, or imported
// static contents, which aren't any of these).
std::optional<ContentsKind> contents_kind(const model::Volume& volume);
// Swaps one volume's contents and nothing else: to a placeholder; to empty
// modelled contents; to a procedure (a baked volume's own graph back, else
// the starter graph); or baked (`baked_mesh`: the procedure's current
// mesh, required). Swapping to the kind it already is changes nothing.
Command swap_contents(Id volume, ContentsKind kind, std::optional<mesh::Mesh> baked_mesh = std::nullopt);

// --- generated children (L4) ---------------------------------------------------------------------

// A composite's generator to start from: a children.grid of two cells from
// `template_name` (empty: placeholders), to output.children. set_graph()
// on a composite sets its generator; graph_of() reads it.
proc::Graph starter_generator(const std::string& template_name);
// Turns `child`, an authored child of a composite, into a template of that
// composite and deletes it (as remove_volume() would). The template keeps
// the child's id, so its tree's references to its boundary stay valid, and
// `origin` is the min corner of the child's box (from the layout build).
Command make_template(Id child, core::dvec3 origin);
Command set_template_name(Id composite, Id template_id, std::string name);
Command remove_template(Id composite, Id template_id);
// The composite keeps its templates and authored children.
Command clear_generator(Id composite);

// --- connection surfaces, keep-out regions, links -----------------------------------------------

// L3: which child of `composite` meets `surface`, a surface joining the
// composite from outside (null child: none; the surface stops there).
Command set_delegation(Id composite, Id surface, Id child);

Command add_connection(model::ConnectionSurface surface);
Command update_connection(model::ConnectionSurface surface);
Command remove_connection(Id surface);

Command add_keep_out(model::KeepOut keep_out);
Command update_keep_out(model::KeepOut keep_out);
Command remove_keep_out(Id keep_out);

Command add_link(model::Link link);
Command remove_link(Id link);

// Renames the document.
Command rename_document(std::string name);

}  // namespace vs::app

#include "authoring.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <utility>

#include "reference_choices.hpp"
#include "vs/eval/import.hpp"
#include "vs/model/hierarchy.hpp"
#include "vs/proc/bake.hpp"
#include "vs/proc/check.hpp"

namespace vs::app {

namespace {

using core::Diagnostic;
using core::Diagnostics;

Diagnostics error(std::string code, std::string message) {
  Diagnostics d;
  d.add(Diagnostic::error(std::move(code), std::move(message)));
  return d;
}

Diagnostics no_tree() { return error("app.edit.no_tree", "That volume has no feature tree to edit."); }
Diagnostics no_feature() { return error("app.edit.no_feature", "That feature is no longer there."); }

std::string feature_title(const feature::Feature& f) {
  if (!f.name.empty()) return f.name;
  if (const auto* s = f.as<feature::SketchFeature>(); s != nullptr && !s->sketch.name().empty()) {
    return s->sketch.name();
  }
  return kind_title(f.kind());
}

template <typename T, typename Pred>
bool erase_if_any(std::vector<T>& v, Pred pred) {
  const auto before = v.size();
  v.erase(std::remove_if(v.begin(), v.end(), pred), v.end());
  return v.size() != before;
}

// Delegations whose surface or child is gone, or whose surface no longer
// joins the composite (L3).
// A surface passed elsewhere no longer reaches the composites below, so
// theirs go too, level by level.
void drop_stale_delegations(model::Document& doc) {
  for (bool changed = true; changed;) {
    changed = false;
    for (auto& v : doc.volumes) {
      auto* c = std::get_if<model::CompositeContents>(&v.contents);
      if (c == nullptr) continue;
      changed |= erase_if_any(c->delegations, [&](const model::Delegation& d) {
        const model::Volume* child = doc.find_volume(d.child);
        const model::ConnectionSurface* s = doc.find_connection(d.surface);
        return child == nullptr || !(child->parent == v.id) || s == nullptr || !model::joins(doc, *s, v.id);
      });
    }
  }
}

}  // namespace

std::string kind_title(feature::FeatureKind kind) {
  switch (kind) {
    case feature::FeatureKind::Sketch: return "Sketch";
    case feature::FeatureKind::DatumPlane: return "Datum plane";
    case feature::FeatureKind::DatumAxis: return "Datum axis";
    case feature::FeatureKind::DatumPoint: return "Datum point";
    case feature::FeatureKind::Extrude: return "Extrude";
    case feature::FeatureKind::Revolve: return "Revolve";
    case feature::FeatureKind::Sweep: return "Sweep";
    case feature::FeatureKind::Loft: return "Loft";
    case feature::FeatureKind::Shell: return "Shell";
    case feature::FeatureKind::Fillet: return "Fillet";
    case feature::FeatureKind::Chamfer: return "Chamfer";
    case feature::FeatureKind::LinearPattern: return "Linear pattern";
    case feature::FeatureKind::CircularPattern: return "Circular pattern";
  }
  return "Feature";
}

std::string unique_name(const std::string& base, const std::vector<std::string>& taken) {
  auto free = [&](const std::string& n) { return std::find(taken.begin(), taken.end(), n) == taken.end(); };
  if (free(base)) return base;
  for (int i = 2;; ++i) {
    std::string n = fmt::format("{} {}", base, i);
    if (free(n)) return n;
  }
}

// --- features ------------------------------------------------------------------------

Command add_feature(Id volume, feature::Feature f, std::optional<std::size_t> index) {
  std::string label = "Add " + feature_title(f);
  return {std::move(label), [volume, f = std::move(f), index](model::Project& p) -> Diagnostics {
            feature::FeatureTree* tree = tree_of(p.document, volume);
            if (tree == nullptr) return no_tree();
            feature::Feature copy = f;
            if (copy.id.is_null()) {
              if (const auto* s = copy.as<feature::SketchFeature>()) {
                copy.id = s->sketch.id();
              } else {
                copy.id = Id::random();
              }
            }
            tree->insert(index ? *index : tree->features().size(), std::move(copy));
            return {};
          }};
}

Command replace_feature(Id volume, feature::Feature f) {
  std::string label = "Edit " + feature_title(f);
  return {std::move(label), [volume, f = std::move(f)](model::Project& p) -> Diagnostics {
            feature::FeatureTree* tree = tree_of(p.document, volume);
            if (tree == nullptr) return no_tree();
            feature::Feature* existing = tree->find(f.id);
            if (existing == nullptr) return no_feature();
            *existing = f;
            return {};
          }};
}

Command remove_feature(Id volume, Id id) {
  return {"Delete feature", [volume, id](model::Project& p) -> Diagnostics {
            feature::FeatureTree* tree = tree_of(p.document, volume);
            if (tree == nullptr) return no_tree();
            if (!tree->remove(id)) return no_feature();
            return {};
          }};
}

Command move_feature(Id volume, Id id, std::size_t index) {
  return {"Move feature", [volume, id, index](model::Project& p) -> Diagnostics {
            feature::FeatureTree* tree = tree_of(p.document, volume);
            if (tree == nullptr) return no_tree();
            if (!tree->move(id, index)) return no_feature();
            return {};
          }};
}

Command set_suppressed(Id volume, Id id, bool suppressed) {
  return {suppressed ? "Suppress feature" : "Unsuppress feature",
          [volume, id, suppressed](model::Project& p) -> Diagnostics {
            feature::FeatureTree* tree = tree_of(p.document, volume);
            if (tree == nullptr) return no_tree();
            if (!tree->set_suppressed(id, suppressed)) return no_feature();
            return {};
          }};
}

Command rename_feature(Id volume, Id id, std::string name) {
  return {"Rename feature", [volume, id, name = std::move(name)](model::Project& p) -> Diagnostics {
            feature::FeatureTree* tree = tree_of(p.document, volume);
            if (tree == nullptr) return no_tree();
            feature::Feature* f = tree->find(id);
            if (f == nullptr) return no_feature();
            f->name = name;
            return {};
          }};
}

Command set_sketch(Id volume, Id id, sketch::Sketch sk) {
  std::string label = "Edit sketch" + (sk.name().empty() ? std::string() : " " + sk.name());
  return {std::move(label), [volume, id, sk = std::move(sk)](model::Project& p) -> Diagnostics {
            feature::FeatureTree* tree = tree_of(p.document, volume);
            if (tree == nullptr) return no_tree();
            sketch::Sketch* existing = tree->sketch(id);
            if (existing == nullptr) return no_feature();
            *existing = sk;
            return {};
          }};
}

// --- variables -----------------------------------------------------------------------

Command add_variable(Id volume, std::string name, std::string expression) {
  std::string label = "Add variable " + name;
  return {std::move(label), [volume, name = std::move(name), expression = std::move(expression)](
                                model::Project& p) -> Diagnostics {
            feature::FeatureTree* tree = tree_of(p.document, volume);
            if (tree == nullptr) return no_tree();
            auto added = tree->variables().add(name, expression);
            if (!added) {
              Diagnostics d;
              d.add(added.error());
              return d;
            }
            return {};
          }};
}

Command set_variable(Id volume, Id variable, std::string name, std::string expression) {
  std::string label = "Edit variable " + name;
  return {std::move(label), [volume, variable, name = std::move(name), expression = std::move(expression)](
                                model::Project& p) -> Diagnostics {
            feature::FeatureTree* tree = tree_of(p.document, volume);
            if (tree == nullptr) return no_tree();
            auto& table = tree->variables();
            const feature::Variable* v = table.find(variable);
            if (v == nullptr) return error("app.edit.no_variable", "That variable is no longer there.");
            if (v->name != name) {
              // Renaming: rebuild the table with the new name, keeping order
              // and id, and check it as a whole (a rename can't be checked by
              // set_expression()).
              if (!feature::is_valid_name(name)) {
                return error("feature.variable.bad_name", fmt::format("'{}' isn't a legal variable name.", name));
              }
              if (table.find(name) != nullptr) {
                return error("feature.variable.duplicate_name",
                             fmt::format("There is already a variable named '{}'.", name));
              }
              feature::VariableTable renamed;
              for (const auto& var : table.variables()) {
                feature::Variable copy = var;
                if (copy.id == variable) copy.name = name;
                renamed.add_unchecked(std::move(copy));
              }
              table = std::move(renamed);
            }
            Diagnostics d = table.set_expression(variable, expression);
            if (d.has_errors()) return d;
            Diagnostics check = table.check();
            return check;
          }};
}

Command remove_variable(Id volume, Id variable) {
  return {"Delete variable", [volume, variable](model::Project& p) -> Diagnostics {
            feature::FeatureTree* tree = tree_of(p.document, volume);
            if (tree == nullptr) return no_tree();
            if (!tree->variables().remove(variable)) {
              return error("app.edit.no_variable", "That variable is no longer there.");
            }
            return {};
          }};
}

// --- volumes -------------------------------------------------------------------------

model::Volume volume_from_features(std::string name, std::vector<Id> features, Id parent) {
  model::Volume v;
  v.id = Id::random();
  v.name = std::move(name);
  v.parent = parent;
  v.boundary = model::FeatureBoundary{std::move(features)};
  v.contents = model::StaticContents{};
  return v;
}

model::Volume volume_from_box(std::string name, feature::PointRef min, feature::PointRef max, Id parent) {
  model::Volume v;
  v.id = Id::random();
  v.name = std::move(name);
  v.parent = parent;
  v.boundary = model::BoxBoundary{std::move(min), std::move(max)};
  v.contents = model::StaticContents{};
  return v;
}

Command add_volume(model::Volume volume) {
  std::string label = "Add volume " + volume.name;
  return {std::move(label), [volume = std::move(volume)](model::Project& p) -> Diagnostics {
            p.document.volumes.push_back(volume);
            return {};
          }};
}

Command update_volume(model::Volume volume) {
  std::string label = "Edit volume " + volume.name;
  return {std::move(label), [volume = std::move(volume)](model::Project& p) -> Diagnostics {
            model::Volume* v = p.document.find_volume(volume.id);
            if (v == nullptr) return error("app.edit.no_volume", "That volume is no longer there.");
            *v = volume;
            return {};
          }};
}

Command remove_volume(Id volume) {
  return {"Delete volume", [volume](model::Project& p) -> Diagnostics {
            model::Document& doc = p.document;
            if (doc.find_volume(volume) == nullptr) {
              return error("app.edit.no_volume", "That volume is no longer there.");
            }
            // A composite goes with everything inside it (L3).
            std::vector<Id> gone{volume};
            for (const auto& v : doc.volumes) {
              if (model::is_within(doc, v.id, volume) && !(v.id == volume)) gone.push_back(v.id);
            }
            const auto is_gone = [&](const Id& id) { return std::find(gone.begin(), gone.end(), id) != gone.end(); };
            erase_if_any(doc.volumes, [&](const model::Volume& v) { return is_gone(v.id); });
            erase_if_any(doc.links, [&](const model::Link& l) {
              if (is_gone(l.source.volume)) return true;
              // A link driving one of its parameters.
              return std::none_of(doc.volumes.begin(), doc.volumes.end(),
                                  [&](const model::Volume& v) {
                                    const auto* t = v.composite() ? v.tree() : nullptr;  // L3
                                    return v.find_parameter(l.target) != nullptr ||
                                           (t != nullptr && t->variables().find(l.target) != nullptr);
                                  }) &&
                     doc.layout.variables().find(l.target) == nullptr;
            });
            for (auto& s : doc.connections) erase_if_any(s.volumes, is_gone);
            erase_if_any(doc.connections, [](const model::ConnectionSurface& s) { return s.volumes.empty(); });
            for (auto& k : doc.keep_outs) {
              erase_if_any(k.volumes, is_gone);
              erase_if_any(k.cutters, is_gone);
            }
            erase_if_any(doc.keep_outs,
                         [](const model::KeepOut& k) { return k.volumes.empty() || k.cutters.empty(); });
            drop_stale_delegations(doc);
            return {};
          }};
}

Command set_keep_out(Id volume, bool keep_out) {
  return {keep_out ? "Mark keep-out" : "Unmark keep-out", [volume, keep_out](model::Project& p) -> Diagnostics {
            model::Volume* v = p.document.find_volume(volume);
            if (v == nullptr) return error("app.edit.no_volume", "That volume is no longer there.");
            if (keep_out && !v->contents_empty()) {
              return error("app.edit.keep_out_contents",
                           fmt::format("{} has contents; a keep-out is empty space. Clear its contents first.",
                                       v->name));
            }
            v->keep_out = keep_out;
            if (keep_out) {
              // Keep-outs join no connection surface.
              for (auto& s : p.document.connections) {
                if (model::joins(p.document, s, volume)) {
                  return error("app.edit.keep_out_connected",
                               fmt::format("{} is joined by {}; remove the surface first.", v->name, s.name));
                }
              }
            }
            return {};
          }};
}

Command make_parametric(Id volume) {
  return {"Fill by modelling", [volume](model::Project& p) -> Diagnostics {
            model::Volume* v = p.document.find_volume(volume);
            if (v == nullptr) return error("app.edit.no_volume", "That volume is no longer there.");
            if (v->keep_out) return error("app.edit.keep_out_contents", "A keep-out has no contents.");
            if (v->tree() != nullptr) return {};
            v->contents = model::ParametricContents{};
            return {};
          }};
}

Command make_placeholder(Id volume) {
  return {"Clear contents", [volume](model::Project& p) -> Diagnostics {
            model::Volume* v = p.document.find_volume(volume);
            if (v == nullptr) return error("app.edit.no_volume", "That volume is no longer there.");
            v->contents = model::StaticContents{};
            return {};
          }};
}

Command import_contents(Id volume, std::vector<std::byte> file, std::string format,
                        core::ProductSettings settings) {
  return {"Import contents", [volume, file = std::move(file), format = std::move(format),
                              settings](model::Project& p) -> Diagnostics {
            return eval::import_contents(p, volume, file, format, settings);
          }};
}

Command import_volume(std::vector<std::byte> file, std::string format, std::string name,
                      core::ProductSettings settings) {
  return {"Import " + name, [file = std::move(file), format = std::move(format), name,
                              settings](model::Project& p) -> Diagnostics {
            auto id = eval::import_volume(p, file, format, name, settings);
            Diagnostics d;
            if (!id) d.add(id.error());
            return d;
          }};
}

// --- procedural contents (M15) ---------------------------------------------------------------

proc::Graph starter_graph() {
  proc::Graph g;
  const Id boundary = g.add("input.boundary", "Boundary");
  const Id output = g.add("output", "Contents");
  g.link(boundary, "field", output, "contents");
  return g;
}

proc::Graph starter_generator(const std::string& template_name) {
  proc::Graph g;
  const Id grid = g.add("children.grid", "Grid",
                        {{"count", proc::Setting(core::dvec3{2, 1, 1})},
                         {"template", proc::Setting(template_name)}});
  const Id output = g.add("output.children", "Children");
  g.link(grid, "children", output, "children");
  return g;
}

namespace {

// Errors check_volume_graph() finds in `graph` for `volume`, but the ones
// that only say the graph is unfinished.
Diagnostics graph_errors(const proc::Graph& graph, const model::Document& doc, const Id& volume) {
  Diagnostics out;
  for (const auto& d : proc::check_volume_graph(graph, doc, volume)) {
    if (d.is_error() && !proc::is_unfinished(d.code)) out.add(d);
  }
  return out;
}

}  // namespace

Command make_procedural(Id volume, proc::Graph graph, std::string label) {
  return {std::move(label), [volume, graph = std::move(graph)](model::Project& p) -> Diagnostics {
            model::Volume* v = p.document.find_volume(volume);
            if (v == nullptr) return error("app.edit.no_volume", "That volume is no longer there.");
            if (v->keep_out) return error("app.edit.keep_out_contents", "A keep-out has no contents.");
            if (!v->contents_empty() && v->kind() != model::VolumeKind::Procedural) {
              return error("app.edit.has_contents",
                           fmt::format("'{}' has {} contents; clear them to fill it by procedure", v->name,
                                       model::to_string(v->kind())));
            }
            Diagnostics errors = graph_errors(graph, p.document, volume);
            if (errors.has_errors()) return errors;
            v->contents = model::ProceduralContents{proc::to_json(graph)};
            return {};
          }};
}

Command set_graph(Id volume, proc::Graph graph, std::string label) {
  return {std::move(label), [volume, graph = std::move(graph)](model::Project& p) -> Diagnostics {
            model::Volume* v = p.document.find_volume(volume);
            if (v == nullptr) return error("app.edit.no_volume", "That volume is no longer there.");
            auto* pc = std::get_if<model::ProceduralContents>(&v->contents);
            auto* cc = std::get_if<model::CompositeContents>(&v->contents);  // L4: its generator
            if (pc == nullptr && cc == nullptr) {
              return error("app.edit.no_graph", "That volume has no graph to edit.");
            }
            Diagnostics errors = graph_errors(graph, p.document, volume);
            if (errors.has_errors()) return errors;
            (pc != nullptr ? pc->graph : cc->generator) = proc::to_json(graph);
            return {};
          }};
}

std::optional<proc::Graph> graph_of(const model::Document& document, const Id& volume) {
  const model::Volume* v = document.find_volume(volume);
  const auto* pc = v != nullptr ? std::get_if<model::ProceduralContents>(&v->contents) : nullptr;
  const auto* cc = v != nullptr ? std::get_if<model::CompositeContents>(&v->contents) : nullptr;
  if (pc == nullptr && (cc == nullptr || !cc->generates())) return std::nullopt;
  const std::string& text = pc != nullptr ? pc->graph : cc->generator;
  if (text.empty()) return proc::Graph{};
  auto g = proc::graph_from_json(text);
  if (!g) return std::nullopt;
  return std::move(g).value();
}

Command bake_volume(Id volume, mesh::Mesh mesh) {
  return {"Bake", [volume, mesh = std::move(mesh)](model::Project& p) -> Diagnostics {
            return proc::bake(p, volume, mesh);
          }};
}

Command unbake_volume(Id volume) {
  return {"Unbake", [volume](model::Project& p) -> Diagnostics { return proc::unbake(p, volume); }};
}

// --- swapping contents (M16) ------------------------------------------------------------------

std::string_view to_string(ContentsKind kind) {
  switch (kind) {
    case ContentsKind::Placeholder: return "placeholder";
    case ContentsKind::Parametric: return "modelled";
    case ContentsKind::Procedural: return "procedural";
    case ContentsKind::Baked: return "baked";
    case ContentsKind::Composite: return "composite";
  }
  return "?";
}

std::optional<ContentsKind> contents_kind(const model::Volume& v) {
  if (v.keep_out) return std::nullopt;
  if (v.composite()) return ContentsKind::Composite;
  if (proc::is_baked(v)) return ContentsKind::Baked;
  if (v.tree() != nullptr) return ContentsKind::Parametric;
  if (v.kind() == model::VolumeKind::Procedural) return ContentsKind::Procedural;
  if (v.contents_empty()) return ContentsKind::Placeholder;
  return std::nullopt;  // imported static contents
}

Command swap_contents(Id volume, ContentsKind kind, std::optional<mesh::Mesh> baked_mesh) {
  std::string label = fmt::format("Swap contents to {}", to_string(kind));
  return {std::move(label), [volume, kind, baked_mesh = std::move(baked_mesh)](model::Project& p) -> Diagnostics {
            model::Volume* v = p.document.find_volume(volume);
            if (v == nullptr) return error("app.edit.no_volume", "That volume is no longer there.");
            if (v->keep_out) return error("app.edit.keep_out_contents", "A keep-out has no contents.");
            const auto now = contents_kind(*v);
            if (now == kind) return {};
            if (now == ContentsKind::Composite && !model::children(p.document, volume).empty()) {
              return error("app.swap.has_children",
                           fmt::format("'{}' has child volumes; delete them before swapping its contents.", v->name));
            }
            switch (kind) {
              case ContentsKind::Composite: v->contents = model::CompositeContents{}; return {};
              case ContentsKind::Placeholder: v->contents = model::StaticContents{}; return {};
              case ContentsKind::Parametric: v->contents = model::ParametricContents{}; return {};
              case ContentsKind::Procedural:
                if (now == ContentsKind::Baked) return proc::unbake(p, volume);
                v->contents = model::ProceduralContents{proc::to_json(starter_graph())};
                return {};
              case ContentsKind::Baked:
                if (now != ContentsKind::Procedural) {
                  return error("app.swap.not_procedural", fmt::format("Only a procedural volume can be baked; '{}' "
                                                                      "isn't one.", v->name));
                }
                if (!baked_mesh) {
                  return error("app.bake.not_generated",
                               fmt::format("Wait for '{}' to finish generating before baking it.", v->name));
                }
                return proc::bake(p, volume, *baked_mesh);
            }
            return {};
          }};
}

// --- generated children (L4) -------------------------------------------------------------------

Command make_template(Id child, core::dvec3 origin) {
  return {"Make template", [child, origin](model::Project& p) -> Diagnostics {
            model::Document& doc = p.document;
            const model::Volume* v = doc.find_volume(child);
            if (v == nullptr) return error("app.edit.no_volume", "That volume is no longer there.");
            model::Volume* parent = doc.find_volume(v->parent);
            auto* comp = parent != nullptr ? std::get_if<model::CompositeContents>(&parent->contents) : nullptr;
            if (comp == nullptr) {
              return error("app.template.no_composite",
                           fmt::format("'{}' isn't inside a composite, so there's nothing to make it a template of.",
                                       v->name));
            }
            const auto contents = model::as_template_contents(v->contents);
            if (!contents || v->keep_out) {
              return error("app.template.kind",
                           fmt::format("'{}' is a {}; a template's contents are static, modelled or procedural.",
                                       v->name, v->keep_out ? "keep-out" : "composite"));
            }
            std::vector<std::string> taken;
            for (const auto& t : comp->templates) taken.push_back(t.name);
            model::ChildTemplate t;
            t.id = v->id;  // its tree refers to its boundary by this id
            t.name = unique_name(v->name.empty() ? std::string("Template") : v->name, taken);
            t.contents = *contents;
            t.parameters = v->parameters;
            t.outputs = v->outputs;
            t.origin = origin;
            comp->templates.push_back(std::move(t));
            // The volume goes, as Delete would take it.
            return remove_volume(child).change(p);
          }};
}

Command set_template_name(Id composite, Id template_id, std::string name) {
  return {"Rename template", [composite, template_id, name = std::move(name)](model::Project& p) -> Diagnostics {
            model::Volume* v = p.document.find_volume(composite);
            auto* comp = v != nullptr ? std::get_if<model::CompositeContents>(&v->contents) : nullptr;
            if (comp == nullptr) return error("app.edit.not_composite", "That volume isn't a composite.");
            for (auto& t : comp->templates) {
              if (t.id == template_id) {
                t.name = name;
                return {};
              }
            }
            return error("app.template.missing", "That template is no longer there.");
          }};
}

Command remove_template(Id composite, Id template_id) {
  return {"Delete template", [composite, template_id](model::Project& p) -> Diagnostics {
            model::Volume* v = p.document.find_volume(composite);
            auto* comp = v != nullptr ? std::get_if<model::CompositeContents>(&v->contents) : nullptr;
            if (comp == nullptr) return error("app.edit.not_composite", "That volume isn't a composite.");
            erase_if_any(comp->templates, [&](const model::ChildTemplate& t) { return t.id == template_id; });
            return {};
          }};
}

Command clear_generator(Id composite) {
  return {"Stop generating children", [composite](model::Project& p) -> Diagnostics {
            model::Volume* v = p.document.find_volume(composite);
            auto* comp = v != nullptr ? std::get_if<model::CompositeContents>(&v->contents) : nullptr;
            if (comp == nullptr) return error("app.edit.not_composite", "That volume isn't a composite.");
            comp->generator.clear();
            return {};
          }};
}

// --- connection surfaces, keep-outs, links --------------------------------------------------

Command set_delegation(Id composite, Id surface, Id child) {
  return {child.is_null() ? "Stop passing a surface down" : "Pass a surface down",
          [composite, surface, child](model::Project& p) -> Diagnostics {
            model::Volume* v = p.document.find_volume(composite);
            auto* c = v != nullptr ? std::get_if<model::CompositeContents>(&v->contents) : nullptr;
            if (c == nullptr) return error("app.edit.not_composite", "That volume isn't a composite.");
            const model::ConnectionSurface* s = p.document.find_connection(surface);
            if (s == nullptr || !model::joins(p.document, *s, composite)) {
              return error("app.edit.no_connection", "That connection surface doesn't join the composite.");
            }
            erase_if_any(c->delegations, [&](const model::Delegation& d) { return d.surface == surface; });
            if (!child.is_null()) c->delegations.push_back({surface, child});
            drop_stale_delegations(p.document);  // the old route's onward passes
            return {};
          }};
}

Command add_connection(model::ConnectionSurface surface) {
  std::string label = "Add connection surface " + surface.name;
  return {std::move(label), [surface = std::move(surface)](model::Project& p) -> Diagnostics {
            p.document.connections.push_back(surface);
            return {};
          }};
}

Command update_connection(model::ConnectionSurface surface) {
  std::string label = "Edit connection surface " + surface.name;
  return {std::move(label), [surface = std::move(surface)](model::Project& p) -> Diagnostics {
            model::ConnectionSurface* s = p.document.find_connection(surface.id);
            if (s == nullptr) return error("app.edit.no_connection", "That connection surface is no longer there.");
            *s = surface;
            return {};
          }};
}

Command remove_connection(Id surface) {
  return {"Delete connection surface", [surface](model::Project& p) -> Diagnostics {
            if (!erase_if_any(p.document.connections,
                              [&](const model::ConnectionSurface& s) { return s.id == surface; })) {
              return error("app.edit.no_connection", "That connection surface is no longer there.");
            }
            drop_stale_delegations(p.document);
            return {};
          }};
}

Command add_keep_out(model::KeepOut keep_out) {
  std::string label = "Add keep-out region " + keep_out.name;
  return {std::move(label), [keep_out = std::move(keep_out)](model::Project& p) -> Diagnostics {
            p.document.keep_outs.push_back(keep_out);
            return {};
          }};
}

Command update_keep_out(model::KeepOut keep_out) {
  std::string label = "Edit keep-out region " + keep_out.name;
  return {std::move(label), [keep_out = std::move(keep_out)](model::Project& p) -> Diagnostics {
            for (auto& k : p.document.keep_outs) {
              if (k.id == keep_out.id) {
                k = keep_out;
                return {};
              }
            }
            return error("app.edit.no_keep_out", "That keep-out region is no longer there.");
          }};
}

Command remove_keep_out(Id keep_out) {
  return {"Delete keep-out region", [keep_out](model::Project& p) -> Diagnostics {
            if (!erase_if_any(p.document.keep_outs, [&](const model::KeepOut& k) { return k.id == keep_out; })) {
              return error("app.edit.no_keep_out", "That keep-out region is no longer there.");
            }
            return {};
          }};
}

Command add_link(model::Link link) {
  return {"Add link", [link = std::move(link)](model::Project& p) -> Diagnostics {
            p.document.links.push_back(link);
            return {};
          }};
}

Command remove_link(Id link) {
  return {"Delete link", [link](model::Project& p) -> Diagnostics {
            if (!erase_if_any(p.document.links, [&](const model::Link& l) { return l.id == link; })) {
              return error("app.edit.no_link", "That link is no longer there.");
            }
            return {};
          }};
}

Command rename_document(std::string name) {
  return {"Rename project", [name = std::move(name)](model::Project& p) -> Diagnostics {
            p.document.name = name;
            return {};
          }};
}

}  // namespace vs::app

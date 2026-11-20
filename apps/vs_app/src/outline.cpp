#include "outline.hpp"

#include <fmt/format.h>

#include <variant>

#include "vs/proc/graph.hpp"

namespace vs::app {

namespace {

std::string number(double v) { return fmt::format("{:.6g}", v); }

std::string point(const core::dvec3& p) {
  return fmt::format("({:.6g}, {:.6g}, {:.6g})", p.x, p.y, p.z);
}

std::string param_text(const feature::Param& p) {
  if (p.expression.empty()) return number(p.value);
  return fmt::format("{} = {}", p.expression, number(p.value));
}

std::string output_text(const model::OutputValue& v) {
  if (const auto* d = std::get_if<double>(&v)) return number(*d);
  if (const auto* p = std::get_if<core::dvec3>(&v)) return point(*p);
  const auto& b = std::get<core::dbox3>(v);
  return fmt::format("{} .. {}", point(b.min), point(b.max));
}

std::string_view unit_of(model::OutputQuantity q) {
  switch (q) {
    case model::OutputQuantity::Volume: return " mm³";
    case model::OutputQuantity::Mass: return " g";
    case model::OutputQuantity::SurfaceArea: return " mm²";
    case model::OutputQuantity::CentreOfGravity:
    case model::OutputQuantity::Envelope: return " mm";
    case model::OutputQuantity::Published: return "";
  }
  return "";
}

// The volume of a parameter id, if any.
const model::Volume* parameter_owner(const model::Document& doc, const Id& parameter) {
  for (const auto& v : doc.volumes) {
    if (v.find_parameter(parameter) != nullptr) return &v;
  }
  return nullptr;
}

// The feature of `tree` that holds `id`: the feature itself, or the sketch
// holding an entity or constraint of that id.
const feature::Feature* holder(const feature::FeatureTree& tree, const Id& id) {
  if (const auto* f = tree.find(id)) return f;
  for (const auto& f : tree.features()) {
    const auto* s = f.as<feature::SketchFeature>();
    if (s == nullptr) continue;
    for (const auto& e : s->sketch.entities()) {
      if (e.id == id) return &f;
    }
    for (const auto& c : s->sketch.constraints()) {
      if (c.id == id) return &f;
    }
  }
  return nullptr;
}

std::string setting_text(const proc::Setting& s) {
  if (const auto* d = std::get_if<double>(&s)) return number(*d);
  if (const auto* p = std::get_if<core::dvec3>(&s)) return point(*p);
  return std::get<std::string>(s);
}

}  // namespace

Id viewport_subject(const Selection& selection) {
  switch (selection.kind) {
    case Selection::Kind::None: return {};
    case Selection::Kind::Volume:
    case Selection::Kind::ConnectionSurface: return selection.id;
    case Selection::Kind::Feature:
    case Selection::Kind::Node: return selection.volume;
  }
  return {};
}

Selection selection_for(const core::Diagnostic& diagnostic, const model::Document& document) {
  const validate::ActTarget target = validate::route(diagnostic, document);
  const core::SubjectRef& s = target.subject;
  switch (target.on) {
    case validate::ActOn::ConnectionSurface: return Selection::of_surface(s.id);
    case validate::ActOn::Node: return Selection::of_node(s.id, target.volume);
    case validate::ActOn::Contents:
      return Selection::of_volume(target.volume.is_null() ? s.id : target.volume);
    case validate::ActOn::Feature: {
      const feature::FeatureTree* tree = &document.layout;
      if (!target.volume.is_null()) {
        const model::Volume* v = document.find_volume(target.volume);
        tree = v != nullptr ? v->tree() : nullptr;
      }
      if (tree != nullptr) {
        if (const auto* f = holder(*tree, s.id)) return Selection::of_feature(f->id, target.volume);
      }
      return target.volume.is_null() ? Selection::none() : Selection::of_volume(target.volume);
    }
    case validate::ActOn::Layout:
      switch (s.kind) {
        case core::SubjectKind::Volume:
        case core::SubjectKind::Boundary:
          if (document.find_volume(s.id) != nullptr) return Selection::of_volume(s.id);
          break;
        case core::SubjectKind::Parameter:
          if (const auto* v = parameter_owner(document, s.id)) return Selection::of_volume(v->id);
          break;
        case core::SubjectKind::Feature:
          if (document.layout.find(s.id) != nullptr) return Selection::of_feature(s.id);
          break;
        default: break;
      }
      return Selection::none();
  }
  return Selection::none();
}

std::vector<VolumeRow> volume_rows(const eval::Evaluation& evaluation,
                                   const std::map<Id, model::VolumeStatus>& states) {
  std::vector<VolumeRow> rows;
  const model::Document& doc = evaluation.document;
  for (std::size_t i = 0; i < doc.volumes.size(); ++i) {
    const model::Volume& v = doc.volumes[i];
    VolumeRow row;
    row.id = v.id;
    row.name = v.name.empty() ? fmt::format("Volume {}", i + 1) : v.name;
    row.kind = v.kind();
    row.keep_out = v.keep_out;
    row.placeholder = v.contents_empty();
    if (const auto* r = evaluation.find(v.id)) {
      row.status = r->status;
      row.stale_contents = r->stale_contents;
    } else if (const auto* vb = evaluation.build.find_volume(v.id)) {
      row.status = vb->status;
    }
    if (const auto it = states.find(v.id); it != states.end()) row.status = it->second;
    for (const auto& d : evaluation.diagnostics) {
      const auto* t = d.target();
      if (t == nullptr || t->id != v.id) continue;
      if (d.severity == core::Severity::Error) ++row.errors;
      if (d.severity == core::Severity::Warning) ++row.warnings;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<SurfaceRow> surface_rows(const eval::Evaluation& evaluation) {
  std::vector<SurfaceRow> rows;
  const model::Document& doc = evaluation.document;
  for (std::size_t i = 0; i < doc.connections.size(); ++i) {
    const auto& s = doc.connections[i];
    SurfaceRow row;
    row.id = s.id;
    row.name = s.name.empty() ? fmt::format("Surface {}", i + 1) : s.name;
    row.type = s.type();
    for (const auto& vid : s.volumes) {
      const auto* v = doc.find_volume(vid);
      row.volumes.push_back(v != nullptr ? v->name : vid.to_string());
    }
    if (const auto* cb = evaluation.build.find_connection(s.id)) row.ok = cb->ok;
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string kind_label(const VolumeRow& row) {
  if (row.keep_out) return "Keep-out";
  if (row.placeholder) return "Placeholder";
  switch (row.kind) {
    case model::VolumeKind::Static: return "Static";
    case model::VolumeKind::Parametric: return "Parametric";
    case model::VolumeKind::Procedural: return "Procedural";
  }
  return "?";
}

std::string status_label(model::VolumeStatus status) {
  switch (status) {
    case model::VolumeStatus::Current: return "Current";
    case model::VolumeStatus::Stale: return "Stale";
    case model::VolumeStatus::Regenerating: return "Regenerating";
    case model::VolumeStatus::Failed: return "Failed";
  }
  return "?";
}

std::vector<FeatureRow> feature_rows(const eval::Evaluation& evaluation, const Id& volume) {
  std::vector<FeatureRow> rows;
  const model::Document& doc = evaluation.document;
  const feature::FeatureTree* tree = &doc.layout;
  const feature::RebuildResult* rebuild = &evaluation.build.layout;
  if (!volume.is_null()) {
    const model::Volume* v = doc.find_volume(volume);
    if (v == nullptr) return rows;
    if (const auto* pc = std::get_if<model::ProceduralContents>(&v->contents)) {
      auto graph = proc::graph_from_json(pc->graph);
      if (!graph) return rows;
      const eval::VolumeResult* result = evaluation.find(volume);
      for (const auto& n : graph->nodes) {
        // M15: a node the last generation stopped at says why.
        std::string status;
        if (result != nullptr) {
          for (const auto& d : result->diagnostics) {
            if (!d.is_error() || d.subjects.empty() || d.subjects.front().id != n.id) continue;
            status = d.code == "proc.node.infeasible" ? "infeasible" : "failed";
          }
        }
        rows.push_back({Selection::of_node(n.id, volume), n.name.empty() ? n.type : n.name, n.type, status});
      }
      return rows;
    }
    tree = v->tree();
    if (tree == nullptr) return rows;
    rebuild = nullptr;
    if (const auto* r = evaluation.find(volume); r != nullptr && r->contents && r->contents->rebuild) {
      rebuild = &*r->contents->rebuild;
    }
  }
  for (std::size_t i = 0; i < tree->features().size(); ++i) {
    const auto& f = tree->features()[i];
    FeatureRow row;
    row.selection = Selection::of_feature(f.id, volume);
    row.kind = std::string(feature::to_string(f.kind()));
    row.name = f.name;
    if (const auto* s = f.as<feature::SketchFeature>(); row.name.empty() && s != nullptr) row.name = s->sketch.name();
    if (row.name.empty()) row.name = fmt::format("{} {}", row.kind, i + 1);
    if (f.suppressed) {
      row.status = "suppressed";
    } else if (rebuild != nullptr) {
      if (const auto* fr = rebuild->find(f.id)) row.status = std::string(feature::to_string(fr->status));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<Property> inspect(const eval::Evaluation& evaluation, const Selection& selection) {
  std::vector<Property> props;
  const model::Document& doc = evaluation.document;
  auto add = [&](std::string group, std::string name, std::string value) {
    props.push_back({std::move(group), std::move(name), std::move(value)});
  };

  switch (selection.kind) {
    case Selection::Kind::None: {
      add("Project", "Name", doc.name);
      add("Project", "Volumes", std::to_string(doc.volumes.size()));
      add("Project", "Connection surfaces", std::to_string(doc.connections.size()));
      for (const auto& var : doc.layout.variables().variables()) {
        const auto value = evaluation.build.layout.variables.get(var.name);
        add("Variables", var.name,
            value ? fmt::format("{} = {}", var.expression, number(*value)) : var.expression);
      }
      break;
    }
    case Selection::Kind::Volume: {
      const model::Volume* v = doc.find_volume(selection.id);
      if (v == nullptr) break;
      const eval::VolumeResult* r = evaluation.find(v->id);
      const model::VolumeBuild* vb = evaluation.build.find_volume(v->id);
      VolumeRow row;
      row.kind = v->kind();
      row.keep_out = v->keep_out;
      row.placeholder = v->contents_empty();
      add("Volume", "Name", v->name);
      add("Volume", "Kind", kind_label(row));
      add("Volume", "Status", r != nullptr ? status_label(r->status) : "-");
      if (r != nullptr && r->stale_contents) add("Volume", "Contents", "stale (last valid)");
      add("Volume", "Boundary",
          std::holds_alternative<model::BoxBoundary>(v->boundary) ? "box" : "features");
      if (const auto* sc = std::get_if<model::StaticContents>(&v->contents); sc && !sc->blob.empty()) {
        add("Volume", "File", fmt::format("{} ({})", sc->blob, sc->format));
        if (!sc->baked_graph.empty()) add("Volume", "Baked", "yes (graph kept)");
      }
      add("Volume", "Seed", std::to_string(v->seed));
      if (r != nullptr && r->key) add("Volume", "Cache key", r->key->to_hex().substr(0, 16));
      for (const auto& p : v->parameters) {
        std::string value = param_text(p.value);
        if (vb != nullptr) {
          if (const auto it = vb->parameters.find(p.name); it != vb->parameters.end()) {
            value = p.value.expression.empty() ? number(it->second)
                                               : fmt::format("{} = {}", p.value.expression, number(it->second));
          }
        }
        add("Parameters", p.name, value);
      }
      for (const auto& o : v->outputs) {
        std::string value = "-";
        if (r != nullptr && r->contents) {
          for (const auto& res : r->contents->outputs) {
            if (res.output == o.id && res.value) value = output_text(*res.value) + std::string(unit_of(o.quantity));
          }
        }
        add("Published outputs", fmt::format("{} ({})", o.name, model::to_string(o.quantity)), value);
      }
      break;
    }
    case Selection::Kind::ConnectionSurface: {
      const model::ConnectionSurface* s = doc.find_connection(selection.id);
      if (s == nullptr) break;
      const model::ConnectionBuild* cb = evaluation.build.find_connection(s->id);
      add("Connection surface", "Name", s->name);
      add("Connection surface", "Type", std::string(model::to_string(s->type())));
      std::string joins;
      for (const auto& vid : s->volumes) {
        const auto* v = doc.find_volume(vid);
        if (!joins.empty()) joins += ", ";
        joins += v != nullptr ? v->name : vid.to_string();
      }
      if (s->volumes.size() == 1) joins += ", outside";
      add("Connection surface", "Joins", joins);
      add("Connection surface", "Patch",
          std::holds_alternative<model::SketchPatch>(s->geometry) ? "sketch profile" : "datum rectangle");
      if (cb != nullptr && cb->ok) add("Connection surface", "Area", number(cb->area) + " mm²");
      add("Connection surface", "Seam spacing",
          s->seam.spacing > 0 ? number(s->seam.spacing) + " mm" : "derived");
      if (const auto* c = std::get_if<model::ClearanceParams>(&s->params)) {
        add("Clearance", "Gap",
            (cb != nullptr && cb->gap ? number(*cb->gap) : param_text(c->gap)) + " mm");
      }
      if (const auto* f = std::get_if<model::BoltedFlangeParams>(&s->params)) {
        add("Bolted flange", "Thread", f->thread);
        add("Bolted flange", "Minimum wall",
            (cb != nullptr && cb->min_wall ? number(*cb->min_wall) : param_text(f->min_wall)) + " mm");
        if (const auto* circle = std::get_if<model::BoltCircle>(&f->holes.layout)) {
          add("Bolted flange", "Holes",
              fmt::format("circle of {}, Ø{} mm", param_text(circle->count), param_text(circle->diameter)));
        } else if (const auto* grid = std::get_if<model::BoltGrid>(&f->holes.layout)) {
          add("Bolted flange", "Holes",
              fmt::format("grid {} x {}", param_text(grid->columns), param_text(grid->rows)));
        }
        if (cb != nullptr) add("Bolted flange", "Hole centres", std::to_string(cb->hole_centres.size()));
      }
      break;
    }
    case Selection::Kind::Feature: {
      for (const auto& row : feature_rows(evaluation, selection.volume)) {
        if (row.selection != selection) continue;
        add("Feature", "Name", row.name);
        add("Feature", "Kind", row.kind);
        if (!row.status.empty()) add("Feature", "Status", row.status);
      }
      if (!selection.volume.is_null()) {
        if (const auto* v = doc.find_volume(selection.volume)) add("Feature", "In volume", v->name);
      } else {
        add("Feature", "In", "layout");
      }
      break;
    }
    case Selection::Kind::Node: {
      const model::Volume* v = doc.find_volume(selection.volume);
      const auto* pc = v != nullptr ? std::get_if<model::ProceduralContents>(&v->contents) : nullptr;
      if (pc == nullptr) break;
      auto graph = proc::graph_from_json(pc->graph);
      if (!graph) break;
      const proc::Node* n = graph->find_node(selection.id);
      if (n == nullptr) break;
      add("Node", "Name", n->name);
      add("Node", "Type", n->type);
      add("Node", "In volume", v->name);
      for (const auto& [key, value] : n->settings) add("Settings", key, setting_text(value));
      break;
    }
  }

  // Diagnostics about the selection itself.
  const Id subject = selection.id;
  if (!subject.is_null()) {
    for (const auto& d : evaluation.diagnostics) {
      const auto* t = d.target();
      if (t == nullptr || t->id != subject || d.severity == core::Severity::Info) continue;
      add("Diagnostics", d.code, d.message);
    }
  }
  return props;
}

}  // namespace vs::app

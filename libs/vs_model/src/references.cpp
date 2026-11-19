#include "vs/model/references.hpp"

#include <fmt/format.h>

namespace vs::model {

std::string_view to_string(TargetKind kind) {
  switch (kind) {
    case TargetKind::Unknown: return "unknown";
    case TargetKind::LayoutFeature: return "layout feature";
    case TargetKind::LayoutVariable: return "layout variable";
    case TargetKind::Volume: return "volume";
    case TargetKind::ConnectionSurface: return "connection surface";
    case TargetKind::KeepOut: return "keep-out";
    case TargetKind::Link: return "link";
    case TargetKind::Parameter: return "parameter";
    case TargetKind::PublishedOutput: return "published output";
    case TargetKind::InContents: return "contents";
  }
  return "unknown";
}

namespace {

bool layout_holds(const feature::FeatureTree& tree, const Id& id) {
  if (tree.find(id)) return true;
  // Sketch entities of layout sketches count as the layout's.
  for (const auto& f : tree.features()) {
    if (const auto* s = f.as<feature::SketchFeature>()) {
      if (s->sketch.id() == id || s->sketch.find_entity(id) || s->sketch.find_constraint(id)) {
        return true;
      }
    }
  }
  return false;
}

std::string volume_label(const Document& doc, const Id& volume) {
  return doc.volume_subject(volume).label;
}

Diagnostic refusal(std::string code, std::string message, const SubjectRef* by,
                   std::vector<SubjectRef> more) {
  std::vector<SubjectRef> subjects;
  if (by) subjects.push_back(*by);
  subjects.insert(subjects.end(), more.begin(), more.end());
  if (by) message = fmt::format("'{}': {}", by->label, message);
  return Diagnostic::error(std::move(code), std::move(message), std::move(subjects));
}

}  // namespace

Target classify(const Document& doc, const Id& id) {
  Target t;
  t.id = id;
  if (id.is_null()) return t;
  if (layout_holds(doc.layout, id)) {
    t.kind = TargetKind::LayoutFeature;
    return t;
  }
  if (doc.layout.variables().find(id)) {
    t.kind = TargetKind::LayoutVariable;
    return t;
  }
  for (const auto& v : doc.volumes) {
    if (v.id == id) {
      t.kind = TargetKind::Volume;
      return t;
    }
    if (v.find_parameter(id)) {
      t.kind = TargetKind::Parameter;
      t.owner = v.id;
      return t;
    }
    if (v.find_output(id)) {
      t.kind = TargetKind::PublishedOutput;
      t.owner = v.id;
      return t;
    }
  }
  if (doc.find_connection(id)) {
    t.kind = TargetKind::ConnectionSurface;
    return t;
  }
  if (doc.find_keep_out(id)) {
    t.kind = TargetKind::KeepOut;
    return t;
  }
  if (doc.find_link(id)) {
    t.kind = TargetKind::Link;
    return t;
  }
  if (const Volume* owner = doc.contents_owner(id)) {
    t.kind = TargetKind::InContents;
    t.owner = owner->id;
  }
  return t;
}

Result<Target> resolve(const Document& doc, const Scope& scope, const Id& id,
                       const SubjectRef* by) {
  const Target t = classify(doc, id);
  if (scope.is_layout()) {
    if (t.kind == TargetKind::InContents) {
      const std::string owner = volume_label(doc, t.owner);
      return refusal("model.reference.contents",
                     fmt::format("it refers to something inside the contents of '{}', which "
                                 "nothing outside the volume may see; refer to its boundary, a "
                                 "connection surface or a published output instead",
                                 owner),
                     by, {doc.volume_subject(t.owner)});
    }
    return t;
  }

  const Id& self = scope.volume;
  const std::string self_label = volume_label(doc, self);
  switch (t.kind) {
    case TargetKind::Unknown:
      return t;
    case TargetKind::InContents:
      if (t.owner == self) return t;
      return refusal("model.reference.contents",
                     fmt::format("the contents of '{}' refer to something inside the contents "
                                 "of '{}'; a volume can't see its neighbours",
                                 self_label, volume_label(doc, t.owner)),
                     by, {doc.volume_subject(t.owner)});
    case TargetKind::Volume:
      if (t.id == self) return t;
      return refusal("model.reference.sibling",
                     fmt::format("the contents of '{}' refer to the volume '{}'; a volume sees "
                                 "only its own boundary and connection surfaces",
                                 self_label, volume_label(doc, t.id)),
                     by, {doc.volume_subject(t.id)});
    case TargetKind::Parameter:
      if (t.owner == self) return t;
      return refusal("model.reference.sibling",
                     fmt::format("the contents of '{}' refer to a parameter of '{}'; give '{}' "
                                 "a parameter of its own and drive it from the layout",
                                 self_label, volume_label(doc, t.owner), self_label),
                     by, {doc.volume_subject(t.owner)});
    case TargetKind::ConnectionSurface: {
      const ConnectionSurface* s = doc.find_connection(t.id);
      if (s && s->joins(self)) return t;
      return refusal("model.reference.not_input",
                     fmt::format("the contents of '{}' refer to the connection surface '{}', "
                                 "which doesn't join '{}'",
                                 self_label, doc.connection_subject(t.id).label, self_label),
                     by, {doc.connection_subject(t.id)});
    }
    case TargetKind::PublishedOutput:
      return refusal("model.reference.not_input",
                     fmt::format("the contents of '{}' refer to a published output; outputs "
                                 "reach a volume only through a link to one of its parameters",
                                 self_label),
                     by, {doc.output_subject({t.owner, t.id})});
    case TargetKind::LayoutFeature:
    case TargetKind::LayoutVariable:
    case TargetKind::KeepOut:
    case TargetKind::Link: {
      const SubjectRef what = t.kind == TargetKind::LayoutFeature
                                  ? doc.layout.subject(t.id)
                              : t.kind == TargetKind::LayoutVariable
                                  ? doc.layout.variables().subject(t.id)
                              : t.kind == TargetKind::KeepOut ? doc.keep_out_subject(t.id)
                                                              : doc.link_subject(t.id);
      return refusal("model.reference.parent",
                     fmt::format("the contents of '{}' refer to '{}' of the layout; contents "
                                 "anchor only to their own boundary and connection surfaces",
                                 self_label, what.label),
                     by, {what});
    }
  }
  return t;
}

Result<Target> resolve(const Document& doc, const Scope& scope, const BoundaryRef& ref,
                       const SubjectRef* by) {
  auto r = resolve(doc, scope, ref.volume, by);
  if (!r) return r;
  if (r->kind != TargetKind::Volume) {
    return refusal("model.reference.broken", "the boundary it refers to isn't a volume's", by,
                   {});
  }
  return r;
}

Result<Target> resolve(const Document& doc, const Scope& scope, const ConnectionSurfaceRef& ref,
                       const SubjectRef* by) {
  auto r = resolve(doc, scope, ref.surface, by);
  if (!r) return r;
  if (r->kind != TargetKind::ConnectionSurface) {
    return refusal("model.reference.broken",
                   "the connection surface it refers to no longer exists", by, {});
  }
  return r;
}

Result<Target> resolve(const Document& doc, const Scope& scope, const PublishedOutputRef& ref,
                       const SubjectRef* by) {
  auto r = resolve(doc, scope, ref.output, by);
  if (!r) return r;
  if (r->kind != TargetKind::PublishedOutput || r->owner != ref.volume) {
    return refusal("model.reference.broken",
                   "the published output it refers to no longer exists", by, {});
  }
  return r;
}

Diagnostics check_variable_name(const Document& doc, const Scope& scope, std::string_view name,
                                const SubjectRef& by) {
  Diagnostics out;
  if (scope.is_layout()) {
    if (doc.layout.variables().find(name)) return out;
    for (const auto& v : doc.volumes) {
      const auto* tree = v.tree();
      if (tree && tree->variables().find(name)) {
        out.add(refusal("model.reference.contents",
                        fmt::format("it uses '{}', a variable inside the contents of '{}', "
                                    "which nothing outside the volume may see",
                                    name, volume_label(doc, v.id)),
                        &by, {doc.volume_subject(v.id)}));
        return out;
      }
    }
    return out;
  }
  const Volume* self = doc.find_volume(scope.volume);
  if (!self) return out;
  const auto* tree = self->tree();
  if ((tree && tree->variables().find(name)) || self->find_parameter(name)) return out;
  if (const auto* var = doc.layout.variables().find(name)) {
    out.add(refusal("model.reference.parent",
                    fmt::format("the contents of '{}' use the layout variable '{}'; add a "
                                "parameter to '{}' driven by it and use that",
                                self->name, name, self->name),
                    &by, {doc.layout.variables().subject(var->id)}));
    return out;
  }
  for (const auto& v : doc.volumes) {
    if (v.id == self->id) continue;
    const auto* other = v.tree();
    if ((other && other->variables().find(name)) || v.find_parameter(name)) {
      out.add(refusal("model.reference.contents",
                      fmt::format("the contents of '{}' use '{}', which belongs to '{}'; a "
                                  "volume can't see its neighbours",
                                  self->name, name, volume_label(doc, v.id)),
                      &by, {doc.volume_subject(v.id)}));
      return out;
    }
  }
  return out;
}

}  // namespace vs::model

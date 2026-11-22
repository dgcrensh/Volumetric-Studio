#include "vs/model/references.hpp"

#include <fmt/format.h>

#include "vs/model/hierarchy.hpp"
#include "vs/model/index.hpp"

#include "lookup.hpp"

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

// Features and the parts of layout sketches count as the layout's (its
// variables are classified on their own).
bool layout_holds(const feature::FeatureTree& tree, const Id& id) {
  const auto t = feature::find_tree_id(tree, id);
  return t && t->kind != feature::TreeIdKind::Variable;
}

template <typename L>
std::string volume_label(const L& look, const Id& volume) {
  return look.volume_subject(volume).label;
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

Target classify(const Document& doc, const Id& id) { return detail::classify_scan(doc, id); }

namespace detail {

Target classify_scan(const Document& doc, const Id& id) {
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

}  // namespace detail

namespace {

// What a level (the root, or a composite) makes of something that lives at
// level `at` (the root, or inside volume `at`'s contents).
enum class Seen { Own, Below, Above, Elsewhere };

template <typename L>

Seen seen_from(const L& look, const Id& level, const Id& at) {
  if (at == level) return Seen::Own;
  if (!at.is_null() && detail::is_within_in(look, at, level)) return Seen::Below;
  if (at.is_null() || detail::is_within_in(look, level, at)) return Seen::Above;
  return Seen::Elsewhere;
}

// The volume whose contents hide something living at `at`, as seen from
// `level`: the child of `level` holding it, else `at` itself.
template <typename L>
Id hider(const L& look, const Id& level, const Id& at) {
  const Id c = detail::child_toward_in(look, level, at);
  return c.is_null() ? at : c;
}

template <typename L>
Diagnostic hidden(const L& look, const Scope& scope, const Id& owner, const SubjectRef* by) {
  const std::string owner_label = volume_label(look, owner);
  if (scope.is_layout()) {
    return refusal("model.reference.contents",
                   fmt::format("it refers to something inside the contents of '{}', which "
                               "nothing outside the volume may see; refer to its boundary, a "
                               "connection surface or a published output instead",
                               owner_label),
                   by, {look.volume_subject(owner)});
  }
  return refusal("model.reference.contents",
                 fmt::format("the contents of '{}' refer to something inside the contents of "
                             "'{}'; a volume can't see inside its children or its neighbours",
                             volume_label(look, scope.volume), owner_label),
                 by, {look.volume_subject(owner)});
}

template <typename L>
Id parent_of(const L& look, const Id& volume) {
  const Volume* v = look.volume(volume);
  return v != nullptr ? v->parent : Id{};
}

template <typename L>
Result<Target> resolve_in(const L& look, const Scope& scope, const Id& id, const SubjectRef* by) {
  const Target t = look.classify(id);
  const Id& self = scope.volume;  // the level: null for the root layout
  const std::string self_label = scope.is_layout() ? "the layout" : volume_label(look, self);

  const auto parent_refusal = [&](const SubjectRef& what) {
    return refusal("model.reference.parent",
                   fmt::format("the contents of '{}' refer to '{}' of the layout around it; "
                               "contents anchor only to their own boundary and connection "
                               "surfaces",
                               self_label, what.label),
                   by, {what});
  };

  switch (t.kind) {
    case TargetKind::Unknown:
      return t;
    case TargetKind::InContents:
    case TargetKind::LayoutFeature:
    case TargetKind::LayoutVariable:
    case TargetKind::KeepOut:
    case TargetKind::Link: {
      // The level the target lives at.
      Id at;
      if (t.kind == TargetKind::InContents) at = t.owner;
      if (t.kind == TargetKind::KeepOut) at = detail::level_of_in(look, *look.keep_out(t.id));
      if (t.kind == TargetKind::Link) at = detail::level_of_in(look, *look.link(t.id));
      switch (seen_from(look, self, at)) {
        case Seen::Own: return t;
        case Seen::Below: return hidden(look, scope, hider(look, self, at), by);
        case Seen::Elsewhere: return hidden(look, scope, at, by);
        case Seen::Above: break;
      }
      SubjectRef what;
      if (t.kind == TargetKind::LayoutFeature) {
        what = look.document().layout.subject(t.id);
      } else if (t.kind == TargetKind::LayoutVariable) {
        what = look.document().layout.variables().subject(t.id);
      } else if (t.kind == TargetKind::KeepOut) {
        what = look.keep_out_subject(t.id);
      } else if (t.kind == TargetKind::Link) {
        what = look.document().link_subject(t.id);
      } else {
        const feature::FeatureTree* tree = look.volume(at)->tree();
        what = tree->variables().find(t.id) ? tree->variables().subject(t.id) : tree->subject(t.id);
      }
      return parent_refusal(what);
    }
    case TargetKind::Volume: {
      if (t.id == self) return t;
      switch (seen_from(look, self, parent_of(look, t.id))) {
        case Seen::Own: return t;
        case Seen::Below: return hidden(look, scope, hider(look, self, parent_of(look, t.id)), by);
        default: break;
      }
      if (detail::is_within_in(look, self, t.id)) return parent_refusal(look.volume_subject(t.id));
      return refusal("model.reference.sibling",
                     fmt::format("the contents of '{}' refer to the volume '{}'; a volume sees "
                                 "only its own boundary and connection surfaces",
                                 self_label, volume_label(look, t.id)),
                     by, {look.volume_subject(t.id)});
    }
    case TargetKind::Parameter: {
      if (t.owner == self) return t;
      switch (seen_from(look, self, parent_of(look, t.owner))) {
        case Seen::Own: return t;
        case Seen::Below:
          return hidden(look, scope, hider(look, self, parent_of(look, t.owner)), by);
        default: break;
      }
      if (detail::is_within_in(look, self, t.owner)) {
        return parent_refusal({core::SubjectKind::Parameter, t.id,
                               fmt::format("a parameter of '{}'", volume_label(look, t.owner))});
      }
      return refusal("model.reference.sibling",
                     fmt::format("the contents of '{}' refer to a parameter of '{}'; give '{}' "
                                 "a parameter of its own and drive it from the layout",
                                 self_label, volume_label(look, t.owner), self_label),
                     by, {look.volume_subject(t.owner)});
    }
    case TargetKind::ConnectionSurface: {
      const ConnectionSurface* s = look.connection(t.id);
      const Id at = s != nullptr ? detail::level_of_in(look, *s) : Id{};
      const Seen seen = seen_from(look, self, at);
      if (s != nullptr && seen == Seen::Own) return t;
      if (s != nullptr && !scope.is_layout() && detail::joins_in(look, *s, self)) return t;
      if (s != nullptr && seen == Seen::Below) return hidden(look, scope, hider(look, self, at), by);
      return refusal("model.reference.not_input",
                     fmt::format("the contents of '{}' refer to the connection surface '{}', "
                                 "which doesn't join '{}'",
                                 self_label, look.connection_subject(t.id).label, self_label),
                     by, {look.connection_subject(t.id)});
    }
    case TargetKind::PublishedOutput: {
      switch (seen_from(look, self, parent_of(look, t.owner))) {
        case Seen::Own: return t;
        case Seen::Below:
          return hidden(look, scope, hider(look, self, parent_of(look, t.owner)), by);
        default: break;
      }
      return refusal("model.reference.not_input",
                     fmt::format("the contents of '{}' refer to a published output; outputs "
                                 "reach a volume only through a link to one of its parameters",
                                 self_label),
                     by, {look.output_subject({t.owner, t.id})});
    }
  }
  return t;
}

}  // namespace

Result<Target> resolve(const Document& doc, const Scope& scope, const Id& id,
                       const SubjectRef* by) {
  return resolve_in(detail::Scan{doc}, scope, id, by);
}

Result<Target> resolve(const DocumentIndex& index, const Scope& scope, const Id& id,
                       const SubjectRef* by) {
  return resolve_in(index, scope, id, by);
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

namespace {

template <typename L>
Diagnostics check_variable_name_in(const L& look, const Scope& scope, std::string_view name,
                                   const SubjectRef& by) {
  const Document& doc = look.document();
  Diagnostics out;
  const Volume* me = scope.is_layout() ? nullptr : look.volume(scope.volume);
  if (!scope.is_layout() && me == nullptr) return out;
  // The level's own names: its layout's variables, and inside a volume its
  // parameters.
  const feature::FeatureTree* own = me != nullptr ? me->tree() : &doc.layout;
  if (own != nullptr && own->variables().find(name)) return out;
  if (me != nullptr && me->find_parameter(name)) return out;

  if (me != nullptr) {
    // A name of a composite around this volume (L3): the parent's geometry.
    for (const Id& level : detail::ancestors_in(look, me->id)) {
      const Volume* a = look.volume(level);
      const feature::FeatureTree* t = a != nullptr ? a->tree() : nullptr;
      if ((t != nullptr && t->variables().find(name)) || (a != nullptr && a->find_parameter(name))) {
        out.add(refusal("model.reference.parent",
                        fmt::format("the contents of '{}' use '{}' of '{}', the volume around "
                                    "it; add a parameter to '{}' driven by it and use that",
                                    me->name, name, a->name, me->name),
                        &by, {look.volume_subject(level)}));
        return out;
      }
    }
    if (const auto* var = doc.layout.variables().find(name)) {
      out.add(refusal("model.reference.parent",
                      fmt::format("the contents of '{}' use the layout variable '{}'; add a "
                                  "parameter to '{}' driven by it and use that",
                                  me->name, name, me->name),
                      &by, {doc.layout.variables().subject(var->id)}));
      return out;
    }
  }

  // A name inside some other volume's contents. From the root layout only
  // their variables count: a layout expression never names parameters.
  for (const auto& v : doc.volumes) {
    if (me != nullptr && v.id == me->id) continue;
    const auto* other = v.tree();
    const bool has =
        (other && other->variables().find(name)) || (me != nullptr && v.find_parameter(name));
    if (!has) continue;
    if (me == nullptr) {
      out.add(refusal("model.reference.contents",
                      fmt::format("it uses '{}', a variable inside the contents of '{}', "
                                  "which nothing outside the volume may see",
                                  name, volume_label(look, v.id)),
                      &by, {look.volume_subject(v.id)}));
    } else {
      out.add(refusal("model.reference.contents",
                      fmt::format("the contents of '{}' use '{}', which belongs to '{}'; a "
                                  "volume can't see its neighbours",
                                  me->name, name, volume_label(look, v.id)),
                      &by, {look.volume_subject(v.id)}));
    }
    return out;
  }
  return out;
}

}  // namespace

Diagnostics check_variable_name(const Document& doc, const Scope& scope, std::string_view name,
                                const SubjectRef& by) {
  return check_variable_name_in(detail::Scan{doc}, scope, name, by);
}

Diagnostics check_variable_name(const DocumentIndex& index, const Scope& scope,
                                std::string_view name, const SubjectRef& by) {
  return check_variable_name_in(index, scope, name, by);
}

}  // namespace vs::model

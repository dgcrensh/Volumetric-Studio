#include "vs/validate/registry.hpp"
#include "vs/model/hierarchy.hpp"

#include <algorithm>
#include <array>

namespace vs::validate {

using core::SubjectKind;

std::string_view to_string(ActOn where) {
  switch (where) {
    case ActOn::Layout: return "layout";
    case ActOn::ConnectionSurface: return "connection surface";
    case ActOn::Feature: return "feature";
    case ActOn::Node: return "node";
    case ActOn::Contents: return "contents";
  }
  return "layout";
}

namespace {

constexpr std::array kCodes{
    CodeInfo{"validate.overlap", Severity::Error, ActOn::Layout,
             "Two sibling boundaries overlap by more than the tolerance"},
    CodeInfo{"validate.contact", Severity::Warning, ActOn::Layout,
             "Faces of two siblings touch, with no connection surface between them"},
    CodeInfo{"validate.contact.edge", Severity::Warning, ActOn::Layout,
             "Two siblings touch along an edge or at a point, with no connection surface"},
    CodeInfo{"validate.connection.off_boundary", Severity::Error, ActOn::ConnectionSurface,
             "A connection surface doesn't lie on a volume it joins"},
    CodeInfo{"validate.connection.gap", Severity::Error, ActOn::ConnectionSurface,
             "A clearance's gap is below the minimum feature size"},
    CodeInfo{"validate.connection.min_wall", Severity::Error, ActOn::ConnectionSurface,
             "A bolted flange's minimum wall is below the minimum feature size"},
    CodeInfo{"validate.connection.edge_wall", Severity::Error, ActOn::ConnectionSurface,
             "A bolt hole leaves less than the minimum wall to the flange's edge"},
    CodeInfo{"validate.connection.hole_spacing", Severity::Error, ActOn::ConnectionSurface,
             "Neighbouring bolt holes leave less than the minimum wall between them"},
    CodeInfo{"validate.nesting.outside", Severity::Error, ActOn::Layout,
             "A child volume's boundary reaches outside its composite's boundary"},
    CodeInfo{"validate.containment", Severity::Error, ActOn::Contents,
             "Contents cross their volume's boundary by more than the tolerance"},
    CodeInfo{"validate.undecided", Severity::Error, ActOn::Layout,
             "The kernel couldn't decide a check; the result is unknown"},
};

bool is_feature_kind(SubjectKind kind) {
  return kind == SubjectKind::Feature || kind == SubjectKind::Sketch ||
         kind == SubjectKind::SketchEntity || kind == SubjectKind::Constraint;
}

// The level a layout-wide subject belongs to (L3): null for the root, else
// the composite whose layout it is built from.
Id level_of_subject(const SubjectRef& s, const model::Document& doc) {
  switch (s.kind) {
    case SubjectKind::Volume:
    case SubjectKind::Boundary:
      if (const auto* v = doc.find_volume(s.id)) return v->parent;
      break;
    case SubjectKind::KeepOut:
      if (const auto* k = doc.find_keep_out(s.id)) return model::level_of(doc, *k);
      break;
    case SubjectKind::Link:
      if (const auto* l = doc.find_link(s.id)) return model::level_of(doc, *l);
      break;
    case SubjectKind::Parameter:
      for (const auto& v : doc.volumes) {
        if (v.find_parameter(s.id)) return v.parent;
      }
      break;
    default:
      break;
  }
  return {};
}

}  // namespace

std::span<const CodeInfo> registered_codes() { return kCodes; }

const CodeInfo* find_code(std::string_view code) {
  const auto it = std::find_if(kCodes.begin(), kCodes.end(),
                               [&](const CodeInfo& c) { return c.code == code; });
  return it == kCodes.end() ? nullptr : &*it;
}

ActTarget route(const Diagnostic& diagnostic, const model::Document& document) {
  ActTarget t;
  const SubjectRef* first = diagnostic.target();
  if (!first) {
    t.subject = {SubjectKind::Layout, {}, "layout"};
    return t;
  }
  t.subject = *first;

  if (const CodeInfo* info = find_code(diagnostic.code)) {
    t.on = info->act_on;
    if (t.on == ActOn::Contents) {
      // The volume is the subject itself.
      t.volume = first->kind == SubjectKind::Volume ? first->id : Id{};
    }
    if (t.on == ActOn::Layout) t.volume = level_of_subject(*first, document);
    return t;
  }

  const model::Volume* owner = document.contents_owner(first->id);
  if (is_feature_kind(first->kind)) {
    t.on = ActOn::Feature;
    if (owner) t.volume = owner->id;
  } else if (first->kind == SubjectKind::ConnectionSurface) {
    t.on = ActOn::ConnectionSurface;
  } else if (first->kind == SubjectKind::Node) {
    t.on = ActOn::Node;
    // Graph nodes live in a volume's graph JSON, which the document can't
    // see into; vs_proc names the volume as a later subject (M10).
    for (const auto& s : diagnostic.subjects) {
      if (s.kind == SubjectKind::Volume) {
        t.volume = s.id;
        break;
      }
    }
  } else if (first->kind == SubjectKind::Variable && owner) {
    t.on = ActOn::Feature;
    t.volume = owner->id;
  } else if (first->kind == SubjectKind::Volume && (diagnostic.code.starts_with("model.contents.") ||
                                                     diagnostic.code.starts_with("eval.") ||
                                                     diagnostic.code.starts_with("proc."))) {
    t.on = ActOn::Contents;
    t.volume = first->id;
  } else {
    t.on = ActOn::Layout;
    t.volume = level_of_subject(*first, document);
  }
  return t;
}

std::vector<RoutedDiagnostic> route_all(const Diagnostics& diagnostics,
                                        const model::Document& document) {
  std::vector<RoutedDiagnostic> out;
  out.reserve(diagnostics.size());
  for (const auto& d : diagnostics) out.push_back({d, route(d, document)});
  return out;
}

}  // namespace vs::validate

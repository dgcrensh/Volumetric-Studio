#pragma once

// The hierarchy walks and subject labels written once over a lookup
// (decision 0029). hierarchy.cpp and references.cpp run them over `Scan`,
// which searches the document's lists as it always did; DocumentIndex runs
// the same code over its maps. A lookup `L` provides:
//
//   const Document& document() const;
//   const Volume* volume(const Id&) const;            first with the id, as find_volume()
//   const ConnectionSurface* connection(const Id&) const;
//   const KeepOut* keep_out(const Id&) const;
//   const Link* link(const Id&) const;
//   Id delegate_of(const Id& composite, const Id& surface) const;
//   Target classify(const Id&) const;
//   SubjectRef volume_subject(id), boundary_subject(id), connection_subject(id),
//              keep_out_subject(id), output_subject(ref)  as Document's
//
// Keeping one copy of each walk is what makes the index's answers the
// lists' answers; test_index.cpp compares them anyway.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "vs/model/document.hpp"
#include "vs/model/references.hpp"

namespace vs::model::detail {

// classify() over the lists (references.cpp).
Target classify_scan(const Document& doc, const Id& id);

// Searches the document's lists, first match wins.
struct Scan {
  const Document& doc;

  [[nodiscard]] const Document& document() const { return doc; }
  [[nodiscard]] const Volume* volume(const Id& id) const { return doc.find_volume(id); }
  [[nodiscard]] const ConnectionSurface* connection(const Id& id) const {
    return doc.find_connection(id);
  }
  [[nodiscard]] const KeepOut* keep_out(const Id& id) const { return doc.find_keep_out(id); }
  [[nodiscard]] const Link* link(const Id& id) const { return doc.find_link(id); }
  [[nodiscard]] Target classify(const Id& id) const { return classify_scan(doc, id); }
  [[nodiscard]] SubjectRef volume_subject(const Id& id) const { return doc.volume_subject(id); }
  [[nodiscard]] SubjectRef boundary_subject(const Id& id) const {
    return doc.boundary_subject(id);
  }
  [[nodiscard]] SubjectRef connection_subject(const Id& id) const {
    return doc.connection_subject(id);
  }
  [[nodiscard]] SubjectRef keep_out_subject(const Id& id) const {
    return doc.keep_out_subject(id);
  }
  [[nodiscard]] SubjectRef output_subject(const PublishedOutputRef& ref) const {
    return doc.output_subject(ref);
  }
  [[nodiscard]] Id delegate_of(const Id& composite, const Id& surface) const {
    const Volume* v = doc.find_volume(composite);
    if (v == nullptr) return {};
    const auto* c = std::get_if<CompositeContents>(&v->contents);
    if (c == nullptr) return {};
    const Delegation* d = c->delegation(surface);
    return d != nullptr ? d->child : Id{};
  }
};

template <typename L>
const feature::FeatureTree* level_layout_in(const L& look, const Id& level) {
  if (level.is_null()) return &look.document().layout;
  const Volume* v = look.volume(level);
  if (v == nullptr) return nullptr;
  const auto* c = std::get_if<CompositeContents>(&v->contents);
  return c != nullptr ? &c->layout : nullptr;
}

template <typename L>
std::vector<Id> ancestors_in(const L& look, const Id& volume) {
  std::vector<Id> out;
  const std::size_t count = look.document().volumes.size();
  const Volume* v = look.volume(volume);
  while (v != nullptr && !v->parent.is_null()) {
    if (v->parent == volume || std::find(out.begin(), out.end(), v->parent) != out.end()) break;
    out.push_back(v->parent);
    if (out.size() > count) break;
    v = look.volume(v->parent);
  }
  return out;
}

template <typename L>
bool is_within_in(const L& look, const Id& volume, const Id& level) {
  if (level.is_null()) return true;
  const auto up = ancestors_in(look, volume);
  return std::find(up.begin(), up.end(), level) != up.end();
}

template <typename L>
Id child_toward_in(const L& look, const Id& level, const Id& volume) {
  const Volume* v = look.volume(volume);
  if (v == nullptr) return {};
  if (v->parent == level) return volume;
  const auto up = ancestors_in(look, volume);
  // up = parent, grandparent, ...: the one just below `level` holds it.
  for (const Id& id : up) {
    const Volume* a = look.volume(id);
    if (a != nullptr && a->parent == level) return id;
  }
  return {};
}

template <typename L>
Id level_of_in(const L& look, const ConnectionSurface& surface) {
  for (const Id& id : surface.volumes) {
    if (const Volume* v = look.volume(id)) return v->parent;
  }
  return {};
}

template <typename L>
Id level_of_in(const L& look, const KeepOut& keep_out) {
  for (const Id& id : keep_out.volumes) {
    if (const Volume* v = look.volume(id)) return v->parent;
  }
  return {};
}

template <typename L>
Id level_of_in(const L& look, const Link& link) {
  const Volume* v = look.volume(link.source.volume);
  return v != nullptr ? v->parent : Id{};
}

template <typename L>
bool joins_in(const L& look, const ConnectionSurface& surface, const Id& volume) {
  if (surface.joins(volume)) return true;
  // Walk up: each step must be the delegate of the next one up, until a
  // volume the surface names.
  Id at = volume;
  for (const Id& parent : ancestors_in(look, volume)) {
    if (!(look.delegate_of(parent, surface.id) == at)) return false;
    if (surface.joins(parent)) return true;
    at = parent;
  }
  return false;
}

// --- subjects -----------------------------------------------------------------------

inline std::string name_or(const std::string& name, std::string_view fallback, const Id& id) {
  if (!name.empty()) return name;
  return fmt::format("{} {}", fallback, id.to_string().substr(0, 8));
}

// `v` is what the lookup found for `id` (null: missing).
inline SubjectRef volume_subject_of(const Volume* v, const Id& id) {
  return {core::SubjectKind::Volume, id, v ? name_or(v->name, "Volume", id) : "(missing volume)"};
}

inline SubjectRef boundary_subject_of(const Volume* v, const Id& id) {
  return {core::SubjectKind::Boundary, id,
          fmt::format("boundary of '{}'", v ? name_or(v->name, "Volume", id) : "(missing)")};
}

inline SubjectRef connection_subject_of(const ConnectionSurface* s, const Id& id) {
  return {core::SubjectKind::ConnectionSurface, id,
          s ? name_or(s->name, "Connection", id) : "(missing connection surface)"};
}

inline SubjectRef keep_out_subject_of(const KeepOut* k, const Id& id) {
  return {core::SubjectKind::KeepOut, id, k ? name_or(k->name, "Keep-out", id) : "(missing keep-out)"};
}

inline SubjectRef output_subject_of(const Volume* v, const PublishedOutputRef& ref) {
  const PublishedOutput* o = v ? v->find_output(ref.output) : nullptr;
  if (!o) return {core::SubjectKind::PublishedOutput, ref.output, "(missing output)"};
  return {core::SubjectKind::PublishedOutput, ref.output,
          fmt::format("{}.{}", name_or(v->name, "Volume", v->id), o->name)};
}

}  // namespace vs::model::detail

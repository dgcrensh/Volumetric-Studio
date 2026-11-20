#include "vs/model/hierarchy.hpp"

#include <algorithm>

namespace vs::model {

const feature::FeatureTree* level_layout(const Document& doc, const Id& level) {
  if (level.is_null()) return &doc.layout;
  const Volume* v = doc.find_volume(level);
  if (v == nullptr) return nullptr;
  const auto* c = std::get_if<CompositeContents>(&v->contents);
  return c != nullptr ? &c->layout : nullptr;
}

std::vector<std::size_t> children(const Document& doc, const Id& level) {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < doc.volumes.size(); ++i) {
    if (doc.volumes[i].parent == level) out.push_back(i);
  }
  return out;
}

std::vector<Id> ancestors(const Document& doc, const Id& volume) {
  std::vector<Id> out;
  const Volume* v = doc.find_volume(volume);
  while (v != nullptr && !v->parent.is_null()) {
    if (v->parent == volume || std::find(out.begin(), out.end(), v->parent) != out.end()) break;
    out.push_back(v->parent);
    if (out.size() > doc.volumes.size()) break;
    v = doc.find_volume(v->parent);
  }
  return out;
}

bool is_within(const Document& doc, const Id& volume, const Id& level) {
  if (level.is_null()) return true;
  const auto up = ancestors(doc, volume);
  return std::find(up.begin(), up.end(), level) != up.end();
}

std::size_t depth(const Document& doc, const Id& volume) { return ancestors(doc, volume).size(); }

Id child_toward(const Document& doc, const Id& level, const Id& volume) {
  const Volume* v = doc.find_volume(volume);
  if (v == nullptr) return {};
  if (v->parent == level) return volume;
  const auto up = ancestors(doc, volume);
  // up = parent, grandparent, ...: the one just below `level` holds it.
  for (std::size_t i = 0; i < up.size(); ++i) {
    const Volume* a = doc.find_volume(up[i]);
    if (a != nullptr && a->parent == level) return up[i];
  }
  return {};
}

std::vector<std::size_t> level_order(const Document& doc) {
  std::vector<std::size_t> out;
  std::vector<Id> levels{Id{}};
  std::vector<bool> taken(doc.volumes.size(), false);
  while (!levels.empty()) {
    std::vector<Id> next;
    for (const Id& level : levels) {
      for (const std::size_t i : children(doc, level)) {
        if (taken[i]) continue;
        taken[i] = true;
        out.push_back(i);
        if (doc.volumes[i].composite()) next.push_back(doc.volumes[i].id);
      }
    }
    levels = std::move(next);
  }
  // Within one depth the batches above follow their parents' order; put
  // each depth in document order instead, so it doesn't depend on that.
  std::stable_sort(out.begin(), out.end(), [&](std::size_t a, std::size_t b) {
    const std::size_t da = depth(doc, doc.volumes[a].id);
    const std::size_t db = depth(doc, doc.volumes[b].id);
    return da != db ? da < db : a < b;
  });
  return out;
}

Id level_of(const Document& doc, const ConnectionSurface& surface) {
  for (const Id& id : surface.volumes) {
    if (const Volume* v = doc.find_volume(id)) return v->parent;
  }
  return {};
}

Id level_of(const Document& doc, const KeepOut& keep_out) {
  for (const Id& id : keep_out.volumes) {
    if (const Volume* v = doc.find_volume(id)) return v->parent;
  }
  return {};
}

Id level_of(const Document& doc, const Link& link) {
  const Volume* v = doc.find_volume(link.source.volume);
  return v != nullptr ? v->parent : Id{};
}

Id delegate_of(const Document& doc, const Id& composite, const Id& surface) {
  const Volume* v = doc.find_volume(composite);
  if (v == nullptr) return {};
  const auto* c = std::get_if<CompositeContents>(&v->contents);
  if (c == nullptr) return {};
  const Delegation* d = c->delegation(surface);
  return d != nullptr ? d->child : Id{};
}

bool joins(const Document& doc, const ConnectionSurface& surface, const Id& volume) {
  if (surface.joins(volume)) return true;
  // Walk up: each step must be the delegate of the next one up, until a
  // volume the surface names.
  Id at = volume;
  for (const Id& parent : ancestors(doc, volume)) {
    if (!(delegate_of(doc, parent, surface.id) == at)) return false;
    if (surface.joins(parent)) return true;
    at = parent;
  }
  return false;
}

std::vector<const ConnectionSurface*> joining_surfaces(const Document& doc, const Id& volume) {
  std::vector<const ConnectionSurface*> out;
  for (const auto& c : doc.connections) {
    if (joins(doc, c, volume)) out.push_back(&c);
  }
  return out;
}

std::vector<Id> seam_sides(const Document& doc, const ConnectionSurface& surface) {
  std::vector<Id> out;
  for (const Id& id : surface.volumes) {
    Id at = id;
    for (std::size_t guard = 0; guard <= doc.volumes.size(); ++guard) {
      const Id next = delegate_of(doc, at, surface.id);
      if (next.is_null()) break;
      at = next;
    }
    out.push_back(at);
  }
  return out;
}

std::string volume_path(const Document& doc, const Id& volume, std::string_view separator) {
  std::string out = doc.volume_subject(volume).label;
  for (const Id& a : ancestors(doc, volume)) {
    out = doc.volume_subject(a).label + std::string(separator) + out;
  }
  return out;
}

}  // namespace vs::model

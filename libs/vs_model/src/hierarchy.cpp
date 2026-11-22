#include "vs/model/hierarchy.hpp"

#include <algorithm>

#include "lookup.hpp"
#include "vs/model/index.hpp"

namespace vs::model {

const feature::FeatureTree* level_layout(const Document& doc, const Id& level) {
  return detail::level_layout_in(detail::Scan{doc}, level);
}

std::vector<std::size_t> children(const Document& doc, const Id& level) {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < doc.volumes.size(); ++i) {
    if (doc.volumes[i].parent == level) out.push_back(i);
  }
  return out;
}

std::vector<Id> ancestors(const Document& doc, const Id& volume) {
  return detail::ancestors_in(detail::Scan{doc}, volume);
}

bool is_within(const Document& doc, const Id& volume, const Id& level) {
  return detail::is_within_in(detail::Scan{doc}, volume, level);
}

std::size_t depth(const Document& doc, const Id& volume) { return ancestors(doc, volume).size(); }

Id child_toward(const Document& doc, const Id& level, const Id& volume) {
  return detail::child_toward_in(detail::Scan{doc}, level, volume);
}

std::vector<std::size_t> level_order(const Document& doc) {
  const DocumentIndex index(doc);
  std::vector<std::size_t> out;
  std::vector<Id> levels{Id{}};
  std::vector<bool> taken(doc.volumes.size(), false);
  while (!levels.empty()) {
    std::vector<Id> next;
    for (const Id& level : levels) {
      for (const std::size_t i : index.children(level)) {
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
  std::vector<std::size_t> depths(doc.volumes.size(), 0);
  for (const std::size_t i : out) depths[i] = index.depth(doc.volumes[i].id);
  std::stable_sort(out.begin(), out.end(), [&](std::size_t a, std::size_t b) {
    return depths[a] != depths[b] ? depths[a] < depths[b] : a < b;
  });
  return out;
}

Id level_of(const Document& doc, const ConnectionSurface& surface) {
  return detail::level_of_in(detail::Scan{doc}, surface);
}

Id level_of(const Document& doc, const KeepOut& keep_out) {
  return detail::level_of_in(detail::Scan{doc}, keep_out);
}

Id level_of(const Document& doc, const Link& link) {
  return detail::level_of_in(detail::Scan{doc}, link);
}

Id delegate_of(const Document& doc, const Id& composite, const Id& surface) {
  return detail::Scan{doc}.delegate_of(composite, surface);
}

bool joins(const Document& doc, const ConnectionSurface& surface, const Id& volume) {
  return detail::joins_in(detail::Scan{doc}, surface, volume);
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

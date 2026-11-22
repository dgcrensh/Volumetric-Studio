#include "vs/model/index.hpp"

#include <algorithm>

#include "lookup.hpp"

namespace vs::model {

namespace {

template <typename T>
void index_ids(const std::vector<T>& items, std::unordered_map<Id, std::size_t>& out) {
  out.reserve(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) out.emplace(items[i].id, i);  // first wins
}

template <typename T>
const T* at(const std::vector<T>& items, const std::unordered_map<Id, std::size_t>& index,
            const Id& id) {
  const auto it = index.find(id);
  return it != index.end() ? &items[it->second] : nullptr;
}

const std::vector<std::size_t> kNone;

}  // namespace

DocumentIndex::DocumentIndex(const Document& document) : doc_(document) {
  index_ids(doc_.volumes, volumes_);
  index_ids(doc_.connections, connections_);
  index_ids(doc_.keep_outs, keep_outs_);
  index_ids(doc_.links, links_);

  // classify(): the order its searches run in, first entry winning, so an id
  // used twice classifies as the search finds it.
  const auto note = [&](const Id& id, TargetKind kind, const Id& owner) {
    if (!id.is_null()) targets_.emplace(id, Target{kind, id, owner});
  };
  // (tree_ids() lists an id where find_tree_id() finds it: variables first.)
  for (const auto& t : feature::tree_ids(doc_.layout)) {
    note(t.id,
         t.kind == feature::TreeIdKind::Variable ? TargetKind::LayoutVariable
                                                 : TargetKind::LayoutFeature,
         {});
  }
  for (const auto& v : doc_.volumes) {
    note(v.id, TargetKind::Volume, {});
    for (const auto& p : v.parameters) note(p.id, TargetKind::Parameter, v.id);
    for (const auto& o : v.outputs) note(o.id, TargetKind::PublishedOutput, v.id);
  }
  for (const auto& c : doc_.connections) note(c.id, TargetKind::ConnectionSurface, {});
  for (const auto& k : doc_.keep_outs) note(k.id, TargetKind::KeepOut, {});
  for (const auto& l : doc_.links) note(l.id, TargetKind::Link, {});
  for (const auto& v : doc_.volumes) {
    if (const auto* t = v.tree()) {
      for (const auto& e : feature::tree_ids(*t)) note(e.id, TargetKind::InContents, v.id);
    }
  }

  // Delegations of the volume each id finds, the first for each surface.
  for (std::size_t i = 0; i < doc_.volumes.size(); ++i) {
    const Volume& v = doc_.volumes[i];
    children_[v.parent].push_back(i);
    if (volumes_.at(v.id) != i) continue;
    if (const auto* c = std::get_if<CompositeContents>(&v.contents)) {
      for (const auto& d : c->delegations) delegations_.emplace(std::make_pair(v.id, d.surface), d.child);
    }
  }

  // joining(): the surfaces naming a volume, and those its parent passes
  // down to it; each candidate is then confirmed by joins() itself.
  std::unordered_map<Id, std::vector<std::size_t>> surfaces_by_id;
  for (std::size_t k = 0; k < doc_.connections.size(); ++k) {
    const ConnectionSurface& c = doc_.connections[k];
    surfaces_by_id[c.id].push_back(k);
    for (const Id& id : c.volumes) joining_[id].push_back(k);
  }
  for (const auto& [key, child] : delegations_) {
    const auto it = surfaces_by_id.find(key.second);
    if (it == surfaces_by_id.end()) continue;
    auto& list = joining_[child];
    list.insert(list.end(), it->second.begin(), it->second.end());
  }
  for (auto& [volume, list] : joining_) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    std::erase_if(list, [&](std::size_t k) { return !joins(doc_.connections[k], volume); });
  }

  for (std::size_t k = 0; k < doc_.keep_outs.size(); ++k) {
    for (const Id& id : doc_.keep_outs[k].volumes) {
      auto& list = keep_outs_of_[id];
      if (list.empty() || list.back() != k) list.push_back(k);
    }
  }
}

const Volume* DocumentIndex::volume(const Id& id) const { return at(doc_.volumes, volumes_, id); }
const ConnectionSurface* DocumentIndex::connection(const Id& id) const {
  return at(doc_.connections, connections_, id);
}
const KeepOut* DocumentIndex::keep_out(const Id& id) const {
  return at(doc_.keep_outs, keep_outs_, id);
}
const Link* DocumentIndex::link(const Id& id) const { return at(doc_.links, links_, id); }

std::optional<std::size_t> DocumentIndex::volume_index(const Id& id) const {
  const auto it = volumes_.find(id);
  if (it == volumes_.end()) return std::nullopt;
  return it->second;
}

SubjectRef DocumentIndex::volume_subject(const Id& v) const {
  return detail::volume_subject_of(volume(v), v);
}
SubjectRef DocumentIndex::boundary_subject(const Id& v) const {
  return detail::boundary_subject_of(volume(v), v);
}
SubjectRef DocumentIndex::connection_subject(const Id& s) const {
  return detail::connection_subject_of(connection(s), s);
}
SubjectRef DocumentIndex::keep_out_subject(const Id& k) const {
  return detail::keep_out_subject_of(keep_out(k), k);
}
SubjectRef DocumentIndex::output_subject(const PublishedOutputRef& ref) const {
  return detail::output_subject_of(volume(ref.volume), ref);
}

const feature::FeatureTree* DocumentIndex::level_layout(const Id& level) const {
  return detail::level_layout_in(*this, level);
}
const std::vector<std::size_t>& DocumentIndex::children(const Id& level) const {
  const auto it = children_.find(level);
  return it != children_.end() ? it->second : kNone;
}
std::vector<Id> DocumentIndex::ancestors(const Id& v) const {
  return detail::ancestors_in(*this, v);
}
bool DocumentIndex::is_within(const Id& v, const Id& level) const {
  return detail::is_within_in(*this, v, level);
}
Id DocumentIndex::child_toward(const Id& level, const Id& v) const {
  return detail::child_toward_in(*this, level, v);
}
Id DocumentIndex::level_of(const ConnectionSurface& s) const { return detail::level_of_in(*this, s); }
Id DocumentIndex::level_of(const KeepOut& k) const { return detail::level_of_in(*this, k); }
Id DocumentIndex::level_of(const Link& l) const { return detail::level_of_in(*this, l); }
Id DocumentIndex::delegate_of(const Id& composite, const Id& surface) const {
  const auto it = delegations_.find({composite, surface});
  return it != delegations_.end() ? it->second : Id{};
}
bool DocumentIndex::joins(const ConnectionSurface& s, const Id& v) const {
  return detail::joins_in(*this, s, v);
}
const std::vector<std::size_t>& DocumentIndex::joining(const Id& v) const {
  const auto it = joining_.find(v);
  return it != joining_.end() ? it->second : kNone;
}
const std::vector<std::size_t>& DocumentIndex::keep_outs_of(const Id& v) const {
  const auto it = keep_outs_of_.find(v);
  return it != keep_outs_of_.end() ? it->second : kNone;
}

Target DocumentIndex::classify(const Id& id) const {
  const auto it = targets_.find(id);
  if (it != targets_.end()) return it->second;
  Target t;
  t.id = id;
  return t;
}

}  // namespace vs::model

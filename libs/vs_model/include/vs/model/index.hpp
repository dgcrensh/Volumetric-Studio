#pragma once

// A read-only index of one document (decision 0029): what the finders,
// hierarchy walks and reference resolution answer, by hash lookups instead
// of searches through the document's lists.
//
// The lists are searched linearly (Document::find_volume() and the free
// functions of hierarchy.hpp and references.hpp), which is right for one
// question and cubic for a pass that asks one per pair of volumes and
// surfaces, as check(), the dependency graph and the layout build do once
// a generator has made thousands of children. Such a pass builds one index
// up front (linear in the document's size) and asks it instead.
//
// Every answer is exactly the list functions' answer, malformed documents
// included: the first item with an id wins, as the searches find it, and
// the walks are the same code (src/lookup.hpp). The index refers to the
// document, which must outlive it and must not change while it is used.

#include <cstddef>
#include <map>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "vs/model/document.hpp"
#include "vs/model/references.hpp"

namespace vs::model {

class DocumentIndex {
 public:
  explicit DocumentIndex(const Document& document);
  DocumentIndex(const DocumentIndex&) = delete;
  DocumentIndex& operator=(const DocumentIndex&) = delete;

  [[nodiscard]] const Document& document() const { return doc_; }

  // Document::find_volume() and friends.
  [[nodiscard]] const Volume* volume(const Id& id) const;
  [[nodiscard]] const ConnectionSurface* connection(const Id& id) const;
  [[nodiscard]] const KeepOut* keep_out(const Id& id) const;
  [[nodiscard]] const Link* link(const Id& id) const;
  // The index of the volume in `document().volumes`, if any.
  [[nodiscard]] std::optional<std::size_t> volume_index(const Id& id) const;

  // Document::volume_subject() and friends.
  [[nodiscard]] SubjectRef volume_subject(const Id& volume) const;
  [[nodiscard]] SubjectRef boundary_subject(const Id& volume) const;
  [[nodiscard]] SubjectRef connection_subject(const Id& surface) const;
  [[nodiscard]] SubjectRef keep_out_subject(const Id& keep_out) const;
  [[nodiscard]] SubjectRef output_subject(const PublishedOutputRef& output) const;

  // hierarchy.hpp.
  [[nodiscard]] const feature::FeatureTree* level_layout(const Id& level) const;
  [[nodiscard]] const std::vector<std::size_t>& children(const Id& level) const;
  [[nodiscard]] std::vector<Id> ancestors(const Id& volume) const;
  [[nodiscard]] bool is_within(const Id& volume, const Id& level) const;
  [[nodiscard]] std::size_t depth(const Id& volume) const { return ancestors(volume).size(); }
  [[nodiscard]] Id child_toward(const Id& level, const Id& volume) const;
  [[nodiscard]] Id level_of(const ConnectionSurface& surface) const;
  [[nodiscard]] Id level_of(const KeepOut& keep_out) const;
  [[nodiscard]] Id level_of(const Link& link) const;
  [[nodiscard]] Id delegate_of(const Id& composite, const Id& surface) const;
  [[nodiscard]] bool joins(const ConnectionSurface& surface, const Id& volume) const;
  // The connection surfaces joining `volume` (joins()), as indices in
  // `document().connections`, in document order.
  [[nodiscard]] const std::vector<std::size_t>& joining(const Id& volume) const;
  // The keep-outs naming `volume` among the volumes they cut, as indices in
  // `document().keep_outs`, in document order.
  [[nodiscard]] const std::vector<std::size_t>& keep_outs_of(const Id& volume) const;

  // references.hpp.
  [[nodiscard]] Target classify(const Id& id) const;

 private:
  const Document& doc_;
  std::unordered_map<Id, std::size_t> volumes_, connections_, keep_outs_, links_;
  std::unordered_map<Id, Target> targets_;
  std::map<std::pair<Id, Id>, Id> delegations_;  // (composite, surface) -> child
  std::unordered_map<Id, std::vector<std::size_t>> children_, joining_, keep_outs_of_;
};

// resolve() and check_variable_name() through an index.
Result<Target> resolve(const DocumentIndex& index, const Scope& scope, const Id& id,
                       const SubjectRef* by = nullptr);
Diagnostics check_variable_name(const DocumentIndex& index, const Scope& scope,
                                std::string_view name, const SubjectRef& by);

}  // namespace vs::model

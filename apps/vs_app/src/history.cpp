#include "history.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "vs/model/check.hpp"

namespace vs::app {

namespace {

using SubjectKey = std::tuple<int, std::string, std::string>;
using ErrorKey = std::pair<std::string, std::vector<SubjectKey>>;

ErrorKey key_of(const core::Diagnostic& d) {
  ErrorKey key{d.code, {}};
  for (const auto& s : d.subjects) {
    key.second.emplace_back(static_cast<int>(s.kind), s.id.to_string(),
                            s.id.is_null() ? s.label : std::string());
  }
  return key;
}

}  // namespace

core::Diagnostics new_errors(const core::Diagnostics& before, const core::Diagnostics& after) {
  std::set<ErrorKey> known;
  for (const auto& d : before) {
    if (d.severity == core::Severity::Error) known.insert(key_of(d));
  }
  core::Diagnostics out;
  for (const auto& d : after) {
    if (d.severity == core::Severity::Error && known.count(key_of(d)) == 0) out.add(d);
  }
  return out;
}

void History::set_undo_limit(std::size_t limit) {
  undo_limit_ = std::max<std::size_t>(limit, 1);
  trim();
}

void History::trim() {
  if (undo_.size() > undo_limit_) {
    undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(undo_.size() - undo_limit_));
  }
}

History::History() : History(model::Project{}) {}

History::History(model::Project initial) { reset(std::move(initial)); }

void History::reset(model::Project project) {
  current_ = std::make_shared<const model::Project>(std::move(project));
  saved_ = current_;
  undo_.clear();
  redo_.clear();
}

const core::Diagnostics& History::current_check() const {
  if (checked_ != current_) {
    checked_diagnostics_ = model::check(current_->document);
    checked_ = current_;
  }
  return checked_diagnostics_;
}

std::shared_ptr<const model::Project> History::try_apply(const Command& command,
                                                         core::Diagnostics* diagnostics) const {
  return attempt(command, diagnostics, nullptr);
}

std::shared_ptr<const model::Project> History::attempt(const Command& command,
                                                       core::Diagnostics* diagnostics,
                                                       core::Diagnostics* found) const {
  auto next = std::make_shared<model::Project>(*current_);
  if (command.change) {
    core::Diagnostics own = command.change(*next);
    if (own.has_errors()) {
      if (diagnostics != nullptr) *diagnostics = std::move(own);
      return nullptr;
    }
  }
  core::Diagnostics after = model::check(next->document);
  core::Diagnostics added = new_errors(current_check(), after);
  if (!added.empty()) {
    if (diagnostics != nullptr) *diagnostics = std::move(added);
    return nullptr;
  }
  // Every error left was there before: report only the rest, so a result
  // with errors always means "refused".
  if (diagnostics != nullptr) {
    *diagnostics = {};
    for (const auto& d : after) {
      if (d.severity != core::Severity::Error) diagnostics->add(d);
    }
  }
  if (found != nullptr) *found = std::move(after);
  return next;
}

core::Diagnostics History::apply(const Command& command) {
  core::Diagnostics d;
  core::Diagnostics found;
  auto next = attempt(command, &d, &found);
  if (!next) return d;
  undo_.push_back({command.label, current_, next});
  trim();
  redo_.clear();
  current_ = std::move(next);
  checked_ = current_;
  checked_diagnostics_ = std::move(found);
  return d;
}

core::Diagnostics History::apply(std::string label, std::function<core::Diagnostics(model::Project&)> change) {
  return apply(Command{std::move(label), std::move(change)});
}

std::string History::undo_label() const { return undo_.empty() ? std::string() : undo_.back().label; }
std::string History::redo_label() const { return redo_.empty() ? std::string() : redo_.back().label; }

bool History::undo() {
  if (undo_.empty()) return false;
  Step step = std::move(undo_.back());
  undo_.pop_back();
  current_ = step.before;
  redo_.push_back(std::move(step));
  return true;
}

bool History::redo() {
  if (redo_.empty()) return false;
  Step step = std::move(redo_.back());
  redo_.pop_back();
  current_ = step.after;
  undo_.push_back(std::move(step));
  trim();
  return true;
}

}  // namespace vs::app

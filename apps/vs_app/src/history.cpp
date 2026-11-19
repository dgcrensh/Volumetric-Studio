#include "history.hpp"

#include <set>
#include <utility>

#include "vs/model/check.hpp"

namespace vs::app {

core::Diagnostics new_errors(const core::Diagnostics& before, const core::Diagnostics& after) {
  std::set<std::string> known;
  for (const auto& d : before) {
    if (d.severity == core::Severity::Error) known.insert(d.to_string());
  }
  core::Diagnostics out;
  for (const auto& d : after) {
    if (d.severity == core::Severity::Error && known.count(d.to_string()) == 0) out.add(d);
  }
  return out;
}

History::History() : History(model::Project{}) {}

History::History(model::Project initial) { reset(std::move(initial)); }

void History::reset(model::Project project) {
  current_ = std::make_shared<const model::Project>(std::move(project));
  saved_ = current_;
  undo_.clear();
  redo_.clear();
}

std::shared_ptr<const model::Project> History::try_apply(const Command& command,
                                                         core::Diagnostics* diagnostics) const {
  auto next = std::make_shared<model::Project>(*current_);
  if (command.change) {
    core::Diagnostics own = command.change(*next);
    if (own.has_errors()) {
      if (diagnostics != nullptr) *diagnostics = std::move(own);
      return nullptr;
    }
  }
  core::Diagnostics after = model::check(next->document);
  core::Diagnostics added = new_errors(model::check(current_->document), after);
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
  return next;
}

core::Diagnostics History::apply(const Command& command) {
  core::Diagnostics d;
  auto next = try_apply(command, &d);
  if (!next) return d;
  undo_.push_back({command.label, current_, next});
  redo_.clear();
  current_ = std::move(next);
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
  return true;
}

}  // namespace vs::app

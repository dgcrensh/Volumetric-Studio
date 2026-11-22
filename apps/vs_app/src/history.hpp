#pragma once

// Undo and redo for every model edit (plan, M14): a command pattern over
// the project.
//
// Every change the app makes to a project is a Command: a label for the
// Edit menu ("Add extrude", "Edit sketch Base outline", ...) and a function
// that changes a project in place. History::apply() runs it on a copy of
// the current project, checks the result (model::check(), as model::edit()
// does) and, unless the change added an error, keeps the copy as the new
// current state and pushes the step on the undo stack.
//
// A step stores the whole project before and after it (as shared,
// immutable snapshots), not an inverse operation. So undo restores exactly
// the project that was there, ids and order included, and its content hash
// (model::content_hash()) is identical: the acceptance test of M14. Copies
// are cheap next to an evaluation: documents are kilobytes, and blobs are
// shared, immutable byte vectors (model::BlobPtr), never copied by a step
// (H5). The undo stack keeps at most undo_limit() steps, dropping the
// oldest.
//
// A document opened with errors (the app opens what the validator opens)
// can still be edited: a change is refused only when it adds an error that
// wasn't there before, so fixing one error at a time works.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vs/core/diagnostic.hpp"
#include "vs/model/project.hpp"

namespace vs::app {

struct Command {
  std::string label;
  // Changes the project in place; errors it returns refuse the change.
  std::function<core::Diagnostics(model::Project&)> change;
};

class History {
 public:
  History();
  explicit History(model::Project initial);

  // Starts over with `project` (opening a file): no undo or redo.
  void reset(model::Project project);

  [[nodiscard]] const model::Project& project() const { return *current_; }
  [[nodiscard]] std::shared_ptr<const model::Project> current() const { return current_; }

  // Applies `command`. On success the change is the new current state and
  // can be undone; the redo stack is dropped, and the result holds check()'s
  // warnings. When the change added an error the project is unchanged and
  // the result holds that error: a result with errors means "refused".
  core::Diagnostics apply(const Command& command);
  core::Diagnostics apply(std::string label, std::function<core::Diagnostics(model::Project&)> change);

  // What apply() would make of `command`, without keeping it: for live
  // previews in dialogs. Null when it would add an error (see `diagnostics`).
  [[nodiscard]] std::shared_ptr<const model::Project> try_apply(const Command& command,
                                                                core::Diagnostics* diagnostics = nullptr) const;

  [[nodiscard]] bool can_undo() const { return !undo_.empty(); }
  [[nodiscard]] bool can_redo() const { return !redo_.empty(); }
  // The label of the step undo() or redo() would take; empty if none.
  [[nodiscard]] std::string undo_label() const;
  [[nodiscard]] std::string redo_label() const;
  [[nodiscard]] std::size_t undo_count() const { return undo_.size(); }
  [[nodiscard]] std::size_t redo_count() const { return redo_.size(); }

  // The deepest the undo stack gets (H5); the oldest steps go first. At
  // least 1.
  static constexpr std::size_t kDefaultUndoLimit = 200;
  [[nodiscard]] std::size_t undo_limit() const { return undo_limit_; }
  void set_undo_limit(std::size_t limit);

  bool undo();
  bool redo();

  // Whether the project changed since mark_saved() (or reset()). Undoing
  // back to the saved state makes it clean again.
  [[nodiscard]] bool dirty() const { return current_ != saved_; }
  void mark_saved() { saved_ = current_; }

 private:
  struct Step {
    std::string label;
    std::shared_ptr<const model::Project> before;
    std::shared_ptr<const model::Project> after;
  };

  std::shared_ptr<const model::Project> current_;
  std::shared_ptr<const model::Project> saved_;
  std::vector<Step> undo_;
  std::vector<Step> redo_;
  std::size_t undo_limit_ = kDefaultUndoLimit;
  // model::check() of one snapshot, `checked_` (decision 0029): snapshots
  // never change, so the current state is checked once, not on every edit.
  mutable std::shared_ptr<const model::Project> checked_;
  mutable core::Diagnostics checked_diagnostics_;

  void trim();
  const core::Diagnostics& current_check() const;
  // try_apply(), also giving the new state's check() findings.
  std::shared_ptr<const model::Project> attempt(const Command& command, core::Diagnostics* diagnostics,
                                                core::Diagnostics* found) const;
};

// The errors in `after` that `before` didn't have: what makes a change
// unacceptable. Errors are compared by code and subjects (kind and id; the
// label only for a subject without an id), never by message (H5): a message
// that names a value or a label changes with an edit that doesn't add the
// error.
core::Diagnostics new_errors(const core::Diagnostics& before, const core::Diagnostics& after);

}  // namespace vs::app

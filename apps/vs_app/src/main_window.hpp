#pragma once

// The main window: the viewport in the middle and dockable panels around
// it (plan, M13), and from M14 everything that authors a project:
//
//   Volumes      the volume tree (kind icons, status badges) and the
//                connection surfaces, coloured by type; its context menu
//                edits, fills, imports, marks keep-outs and deletes
//   Features     the feature tree of the layout, of the selected volume or
//                of the volume edited in context; its context menu edits,
//                renames, suppresses, moves and deletes features
//   Variables    the variables and equations of that tree, editable
//   Inspector    the selection's parameters, connection-surface properties
//                and published outputs
//   Diagnostics  everything the last evaluation, validation or refused
//                edit reported; clicking one selects its "act here" target
//
// Authoring (M14). Every change goes through the History (history.hpp) as
// a Command (authoring.hpp), so Edit > Undo and Redo cover all of them and
// restore identical documents. Dialogs (feature, volume, connection
// surface, keep-out region, link) are modeless: each change in one is
// previewed in the viewport (the edit applied to a copy that isn't kept),
// OK commits it, Cancel puts the project back. Sketch mode replaces the
// viewport with the sketch canvas until Finish or Cancel.
//
// Edit in context: editing a parametric volume's contents (Volume > Edit
// contents) makes its feature tree the one Insert adds to, dims every other
// volume in the viewport and keeps them from being picked, and the
// feature dialogs then offer only the volume's boundary, its connection
// surfaces and its own features as references.

#include <QMainWindow>
#include <QPointer>

#include <filesystem>
#include <functional>
#include <optional>

#include "exporter.hpp"
#include "history.hpp"
#include "outline.hpp"
#include "session.hpp"
#include "widgets.hpp"

class QAction;
class QLabel;
class QMenu;
class QStackedWidget;
class QTableWidget;
class QTimer;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;

namespace vs::app {

class ConnectionDialog;
class FeatureDialog;
class KeepOutDialog;
class LinkDialog;
class SketchMode;
class Viewport;
class VolumeDialog;

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  // Opens a project; false (with the reason shown) when it can't be read.
  bool open_project(const std::filesystem::path& path);
  // Starts an empty project.
  void new_project(const std::string& name = "Untitled");

  void select(const Selection& selection);
  [[nodiscard]] const Selection& selection() const { return selection_; }

  [[nodiscard]] Session& session() { return *session_; }
  [[nodiscard]] Viewport& viewport() { return *viewport_; }
  [[nodiscard]] QTreeWidget& volume_tree() { return *volumes_; }
  [[nodiscard]] QTreeWidget& feature_tree() { return *features_; }
  [[nodiscard]] QTreeWidget& inspector() { return *inspector_; }
  [[nodiscard]] QTreeWidget& diagnostics_list() { return *diagnostics_; }
  [[nodiscard]] QTableWidget& variables_table() { return *variables_; }

  // Exports what is shown, on the worker thread; `done` gets the result
  // on the UI thread after the file is written (when it could be).
  void export_to(const std::filesystem::path& path, ExportFormat format,
                 std::function<void(const ExportResult&)> done = {});

  // Clicks the diagnostics list's row `row`, as the user would.
  void activate_diagnostic(int row);

  // --- authoring (M14). The menus call these; so can tests.
  [[nodiscard]] History& history() { return history_; }
  // Applies an edit: kept and shown, or refused with the reason in the
  // diagnostics list (the result then has errors).
  core::Diagnostics apply(const Command& command);
  void undo();
  void redo();

  // The volume whose contents are edited in context (null: the layout).
  [[nodiscard]] const Id& editing_volume() const { return editing_volume_; }
  // Edits a parametric volume's contents in context (a placeholder is made
  // parametric first). False when the volume can't hold a feature tree.
  bool edit_in_context(const Id& volume);
  void leave_context();

  // A dialog for a new feature of `kind` at the end of the tree being
  // edited, or for an existing feature of `volume`'s tree (null: layout).
  // A sketch's dialog picks its plane; OK then opens sketch mode.
  FeatureDialog* insert_feature(feature::FeatureKind kind);
  FeatureDialog* edit_feature(const Id& volume, const Id& feature);
  // Sketch mode for a new sketch on `plane` in the tree being edited, or
  // for an existing sketch feature.
  SketchMode* new_sketch(const feature::PlaneRef& plane, const std::string& name = {});
  SketchMode* edit_sketch(const Id& volume, const Id& feature);
  [[nodiscard]] SketchMode* sketch_mode() const { return sketch_mode_; }

  VolumeDialog* new_volume();
  VolumeDialog* edit_volume(const Id& volume);
  ConnectionDialog* new_connection();
  ConnectionDialog* edit_connection(const Id& surface);
  KeepOutDialog* new_keep_out();
  LinkDialog* new_link();
  // The authoring dialog open now, if any.
  [[nodiscard]] EditDialog* open_dialog() const { return dialog_; }
  // Whether a dialog's change is shown in place of the project.
  [[nodiscard]] bool previewing() const { return previewing_; }

  // Feature tree actions on a feature of `volume`'s tree (null: layout).
  void move_feature(const Id& volume, const Id& feature, int delta);

  // Waits for the open dialog's pending preview to be applied (tests).
  void flush_preview();

 signals:
  // A full (not preview) evaluation reached the screen.
  void evaluationShown();
  // The history moved (an edit, undo or redo).
  void historyChanged();

 private:
  void build_menus();
  void build_docks();
  void show_snapshot(const SnapshotPtr& snapshot);
  void refresh_volume_tree();
  void refresh_states();
  void refresh_features();
  void refresh_variables();
  void refresh_inspector();
  void refresh_edit_actions();
  void show_diagnostics(const core::Diagnostics& diagnostics, const QString& source);
  void sync_tree_selection();
  void update_title();
  void add_recent(const std::filesystem::path& path);
  void rebuild_recent_menu();
  void open_dialog_box();
  void save();
  void save_as();
  void export_dialog(ExportFormat format);
  void validate();
  void report(const core::Diagnostics& diagnostics, const QString& what);
  void show_project();  // the history's current project, to the session

  // Dialog plumbing: previews on change, commits on OK.
  void start_dialog(EditDialog* dialog, std::function<Command()> make,
                    std::function<bool()> commit_override = {});
  void run_preview();
  void end_preview();
  void close_dialog();

  SketchMode* open_sketch_mode(const Id& volume, feature::Feature feature, bool is_new);
  void finish_sketch(bool accept);

  void feature_context_menu(const QPoint& pos);
  void volume_context_menu(const QPoint& pos);
  void activate_feature_row(QTreeWidgetItem* item);
  [[nodiscard]] std::vector<Id> highlight_for(const Selection& selection) const;
  // The tree the feature panel shows: the context volume, else the
  // selected volume's, else the layout.
  [[nodiscard]] Id shown_tree() const;

  Session* session_ = nullptr;
  History history_;
  Viewport* viewport_ = nullptr;
  QStackedWidget* central_ = nullptr;
  QTreeWidget* volumes_ = nullptr;
  QTreeWidget* features_ = nullptr;
  QTableWidget* variables_ = nullptr;
  QTreeWidget* inspector_ = nullptr;
  QTreeWidget* diagnostics_ = nullptr;
  QMenu* recent_menu_ = nullptr;
  QLabel* status_label_ = nullptr;
  QLabel* frame_label_ = nullptr;
  QTimer* state_timer_ = nullptr;
  QAction* cancel_action_ = nullptr;
  QAction* undo_action_ = nullptr;
  QAction* redo_action_ = nullptr;
  QAction* leave_context_action_ = nullptr;
  QToolBar* context_bar_ = nullptr;
  QLabel* context_label_ = nullptr;

  Selection selection_;
  bool syncing_ = false;          // updating widgets from selection_
  bool fitted_ = false;           // the view was framed for this project
  core::Diagnostics shown_diagnostics_;  // rows of the diagnostics list
  std::map<core::Id, model::VolumeStatus> last_states_;

  Id editing_volume_;

  QPointer<EditDialog> dialog_;
  std::function<Command()> dialog_command_;
  QTimer* preview_timer_ = nullptr;
  bool previewing_ = false;

  SketchMode* sketch_mode_ = nullptr;
  struct SketchSession {
    Id volume;
    feature::Feature feature;
    bool is_new = false;
  };
  std::optional<SketchSession> sketch_session_;
};

}  // namespace vs::app

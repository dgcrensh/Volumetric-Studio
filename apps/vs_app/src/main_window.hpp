#pragma once

// The main window: the viewport in the middle and dockable panels around
// it (plan, M13):
//
//   Volumes      the volume tree (kind icons, status badges) and the
//                connection surfaces, coloured by type
//   Features     the selected volume's feature tree or graph nodes, or the
//                layout's features (read-only until M14)
//   Inspector    the selection's parameters, connection-surface properties
//                and published outputs
//   Diagnostics  everything the last evaluation or validation reported;
//                clicking one selects its "act here" target
//
// Selection is one Selection (outline.hpp) shared by every panel and the
// viewport: choosing in any of them updates the others.
//
// File: open, save, save as, recent files, export STL and 3MF. Model:
// regenerate, cancel, validate. View: fit, standard views, the panels.

#include <QMainWindow>

#include <filesystem>
#include <optional>

#include "exporter.hpp"
#include "outline.hpp"
#include "session.hpp"

class QAction;
class QLabel;
class QMenu;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace vs::app {

class Viewport;

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  // Opens a project; false (with the reason shown) when it can't be read.
  bool open_project(const std::filesystem::path& path);

  void select(const Selection& selection);
  [[nodiscard]] const Selection& selection() const { return selection_; }

  [[nodiscard]] Session& session() { return *session_; }
  [[nodiscard]] Viewport& viewport() { return *viewport_; }
  [[nodiscard]] QTreeWidget& volume_tree() { return *volumes_; }
  [[nodiscard]] QTreeWidget& feature_tree() { return *features_; }
  [[nodiscard]] QTreeWidget& inspector() { return *inspector_; }
  [[nodiscard]] QTreeWidget& diagnostics_list() { return *diagnostics_; }

  // Exports what is shown, on the worker thread; `done` gets the result
  // on the UI thread after the file is written (when it could be).
  void export_to(const std::filesystem::path& path, ExportFormat format,
                 std::function<void(const ExportResult&)> done = {});

  // Clicks the diagnostics list's row `row`, as the user would.
  void activate_diagnostic(int row);

 signals:
  // A full (not preview) evaluation reached the screen.
  void evaluationShown();

 private:
  void build_menus();
  void build_docks();
  void show_snapshot(const SnapshotPtr& snapshot);
  void refresh_volume_tree();
  void refresh_states();
  void refresh_features();
  void refresh_inspector();
  void show_diagnostics(const core::Diagnostics& diagnostics, const QString& source);
  void sync_tree_selection();
  void update_title();
  void add_recent(const std::filesystem::path& path);
  void rebuild_recent_menu();
  void open_dialog();
  void save();
  void save_as();
  void export_dialog(ExportFormat format);
  void validate();
  void report(const core::Diagnostics& diagnostics, const QString& what);

  Session* session_ = nullptr;
  Viewport* viewport_ = nullptr;
  QTreeWidget* volumes_ = nullptr;
  QTreeWidget* features_ = nullptr;
  QTreeWidget* inspector_ = nullptr;
  QTreeWidget* diagnostics_ = nullptr;
  QMenu* recent_menu_ = nullptr;
  QLabel* status_label_ = nullptr;
  QLabel* frame_label_ = nullptr;
  QTimer* state_timer_ = nullptr;
  QAction* cancel_action_ = nullptr;

  Selection selection_;
  bool syncing_ = false;          // updating widgets from selection_
  bool fitted_ = false;           // the view was framed for this project
  core::Diagnostics shown_diagnostics_;  // rows of the diagnostics list
  std::map<core::Id, model::VolumeStatus> last_states_;
};

}  // namespace vs::app

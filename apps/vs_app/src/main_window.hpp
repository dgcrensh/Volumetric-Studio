#pragma once

// The main window: the viewport in the middle and dockable panels around
// it (plan, M13), and from M14 everything that authors a project. Since U1
// phase 4 the Volumes, Features, Variables and Drawings panels are sections
// of one Browser on the left (browser.hpp), and the Browser and the
// Inspector fold to a strip:
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
// Edit in context: editing a parametric volume's contents (the Contents
// tab, or Model on the Layout tab) makes its feature tree the one the
// geometry tools add to, dims every other volume in the viewport and keeps
// them from being picked, and the
// feature dialogs then offer only the volume's boundary, its connection
// surfaces and its own features as references.
//
// Procedural graphs (M15). The Graph dock (node_editor.hpp) edits one
// procedural volume's graph (the Graph tab or the Procedure tool, also
// on a placeholder); every change is a set_graph() Command through the
// same History. Selecting a node shows its settings in the inspector and
// draws its output in the viewport (node_preview.hpp). The template
// library, bake and unbake are tools, and in the volume's context menu.
//
// Interactive regeneration and guidance (M16):
//
//   - Dimension handles: a selected sketch or extrude shows handles on its
//     dimensions (dimension_drag.hpp). Dragging one previews the change
//     without generating (boundaries, untouched contents from the cache);
//     releasing it commits one undoable step and generates.
//   - While an evaluation runs, the status bar shows a progress bar and a
//     Cancel button; contents being replaced stay on screen, grey, until
//     their replacements arrive.
//   - Swap contents (the volume's context menu): placeholder, modelled,
//     procedural or baked, touching nothing outside the volume.
//   - Habit guidance: fillet, cut or mate on generated contents shows an
//     inline explanation with the right action (guidance.hpp).
//   - Pick in view: feature dialogs' Pick buttons let the viewport pick a
//     face or edge (picking.hpp), within edit-in-context's rules.
//
// Configurations (L5): the Configuration box on the context bar's top row
// shows the authored values or one configuration; its Design Table… button
// edits them all as a grid (design_table_dialog.hpp); the inspector marks a
// parameter or variable the shown configuration drives (configurations.hpp).
//
// Drawings (L6): the Browser's Drawings section lists drawings, views and dimensions;
// the drawing window shows one drawing's sheet (drawing_window.hpp),
// recomputed from every evaluation; view and dimension dialogs
// (drawing_dialogs.hpp); the Drawing tab's New and File > Export Drawing…
//
// The context bar (U1, decision 0039): tabs for the context the window is
// in (Layout, Contents, Sketch, Graph, Drawing; tool_catalog.hpp derives it
// from what is open) above that context's toolbar (context_bar.hpp). Every
// tool is one shared QAction (actions.hpp), enabled by what is selected;
// the menus keep File, Edit, View and Help. Clicking a tab enters that
// context the way the tools do (enter_context()).

#include <QMainWindow>
#include <QKeySequence>
#include <QPointer>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "authoring.hpp"
#include "tool_catalog.hpp"
#include "dimension_drag.hpp"
#include "exporter.hpp"
#include "guidance.hpp"
#include "history.hpp"
#include "picking.hpp"
#include "outline.hpp"
#include "session.hpp"
#include "ship.hpp"
#include "vs/eval/step_export.hpp"
#include "widgets.hpp"

class QAction;
class QCloseEvent;
class QComboBox;
class QDockWidget;
class QLabel;
class QMenu;
class QProgressBar;
class QPushButton;
class QSettings;
class QSplitter;
class QStackedWidget;
class QTableWidget;
class QTimer;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;

namespace vs::app {

class ConnectionDialog;
class SvgImportDialog;
class Browser;
class ProblemsButton;
class ProblemsDrawer;
class ContextBar;
class FoldPanel;
class ToolActions;
class DesignTableDialog;
class DimensionDialog;
class DrawingWindow;
class FeatureDialog;
class SheetView;
class ViewDialog;
class KeepOutDialog;
class LinkDialog;
class NodeEditor;
class NodeForm;
class SketchMode;
class TemplateDialog;
struct NodePreviewResult;
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

  // Unsaved changes (H7): File > New, Open, Open Recent and closing the
  // window ask first whether to save them, discard them or stay. The
  // question is a message box unless replaced (tests, scripts).
  enum class UnsavedChoice { Save, Discard, Cancel };
  void set_unsaved_prompt(std::function<UnsavedChoice()> prompt) { unsaved_prompt_ = std::move(prompt); }
  // Asks when there are unsaved changes; false when the user stays (Cancel,
  // or saving didn't happen).
  bool confirm_discard();

  void select(const Selection& selection);
  [[nodiscard]] const Selection& selection() const { return selection_; }

  [[nodiscard]] Session& session() { return *session_; }
  [[nodiscard]] Viewport& viewport() { return *viewport_; }
  [[nodiscard]] QTreeWidget& volume_tree() { return *volumes_; }
  [[nodiscard]] QTreeWidget& feature_tree() { return *features_; }
  [[nodiscard]] QTreeWidget& inspector() { return *inspector_; }
  // U1 phase 4: the left Browser (Volumes, Features, Variables, Drawings
  // sections) and the Inspector's panel, each folding to a strip.
  [[nodiscard]] Browser& browser() { return *browser_; }
  [[nodiscard]] FoldPanel& inspector_panel() { return *inspector_panel_; }
  [[nodiscard]] QTreeWidget& diagnostics_list() { return *diagnostics_; }
  // U1 phase 5: the Problems drawer under the viewport (holding
  // diagnostics_list()) and its count in the status bar.
  [[nodiscard]] ProblemsDrawer& problems_drawer() { return *problems_drawer_; }
  [[nodiscard]] ProblemsButton& problems_button() { return *problems_button_; }
  void set_problems_open(bool open);
  [[nodiscard]] bool problems_open() const;
  // U1 phase 6: the panels' layout (Browser and Inspector widths and
  // folds, open Browser sections, the drawer's height) to and from
  // settings, under "layout/". The app remembers it between runs
  // (set_remember_layout(true): restored now, saved on close); tests pass
  // their own settings file.
  void save_layout(QSettings& settings) const;
  void restore_layout(const QSettings& settings);
  void set_remember_layout(bool remember);
  [[nodiscard]] QTableWidget& variables_table() { return *variables_; }

  // Exports what is shown, on the worker thread; `done` gets the result
  // on the UI thread after the file is written (when it could be).
  void export_to(const std::filesystem::path& path, ExportFormat format,
                 std::function<void(const ExportResult&)> done = {});

  // --- shipping (M17). File > Export runs the same check first.
  // Model > Validate: every check (ship.hpp) against what is shown, on the
  // worker thread; the diagnostics list shows the result and done gets
  // it on the UI thread. Without done (the menu) a summary dialog opens.
  void validate(std::function<void(const ShipCheck&)> done = {});
  [[nodiscard]] const std::optional<ShipCheck>& last_check() const { return last_check_; }
  // Writes what is shown as STEP (eval::export_step()), on the worker.
  void export_step_to(const std::filesystem::path& path, const eval::StepExportOptions& options,
                      std::function<void(const eval::StepExport&)> done = {});
  // File > Import: a STEP, IGES, STL, 3MF or OBJ file as a new static
  // volume named after the file, boxed by its bounds (one undoable step).
  core::Diagnostics import_file(const std::filesystem::path& path);
  // Saves the project to path (a folder when it is one, else a zip), with
  // the shown evaluation's caches when save_caches() is on.
  core::Diagnostics save_to(const std::filesystem::path& path);
  // File > Save Caches With Project (kept between runs; on by default).
  [[nodiscard]] bool save_caches() const;
  void set_save_caches(bool on);

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
  // L4: whether the shown evaluation made `volume` from a composite's
  // procedure (generated volumes are read-only), and Make template: an
  // authored child of a composite becomes one of its templates.
  [[nodiscard]] bool is_generated(const Id& volume) const;
  core::Diagnostics make_template_of(const Id& child);
  void leave_context();
  // L3: where new volumes, connection surfaces, keep-outs and links go: the
  // composite whose layout is edited in context, else the root (null).
  [[nodiscard]] Id authoring_level() const;
  // L3: which child of `composite` meets `surface` (null: none). The
  // connection-surface menu's "Met by" choice.
  bool set_met_by(const Id& composite, const Id& surface, const Id& child);

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
  // S1: Insert > SVG in sketch mode. Asks for a file when `path` is empty,
  // previews the import on the sketch and opens its options; OK keeps it as
  // one sketch step, Cancel takes it out. Null when no sketch is open, no
  // file was chosen, or the file can't be read (said in the status bar).
  SvgImportDialog* import_svg(QString path = {});
  [[nodiscard]] SvgImportDialog* svg_import_dialog() const { return svg_dialog_; }
  // The dock holding the open sketch's status and constraint list (U1).
  [[nodiscard]] QDockWidget& sketch_dock() { return *sketch_dock_; }

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

  // --- procedural graphs (M15). The node editor (the Graph panel) edits
  // one procedural volume's graph at a time.
  // Opens `volume`'s graph in the node editor; a placeholder is first
  // filled with the starter graph (Boundary -> Output). False for other
  // contents (a baked volume says to unbake it first).
  bool edit_graph(const Id& volume);
  void leave_graph();
  [[nodiscard]] const Id& graph_volume() const { return graph_volume_; }
  [[nodiscard]] NodeEditor& node_editor() { return *node_editor_; }
  // The dock holding the node editor: shown in the Graph context only (U1).
  [[nodiscard]] QDockWidget& graph_dock() { return *graph_dock_; }
  [[nodiscard]] NodeForm& node_form() { return *node_form_; }
  // The template library for `volume` (a placeholder or procedural one):
  // OK attaches the chosen template's graph, previewed live meanwhile.
  TemplateDialog* attach_template(const Id& volume);
  // Bakes a procedural volume's current generated mesh (refused until it
  // has generated), or unbakes a baked one: one undoable step each.
  core::Diagnostics bake(const Id& volume);
  core::Diagnostics unbake(const Id& volume);
  // Draws `node`'s output (of the graph being edited) in the viewport;
  // nodePreviewShown() follows when it is on screen. Selecting a node
  // previews it too.
  void preview_node(const Id& node, const std::string& port = {});
  void clear_node_preview();
  [[nodiscard]] std::shared_ptr<const NodePreviewResult> last_node_preview() const { return last_preview_; }

  // --- interactive regeneration and guidance (M16)
  // Dimension handles of the selected feature (empty unless a sketch or
  // extrude is selected); the viewport draws them.
  [[nodiscard]] const std::vector<DimensionHandle>& dimension_handles() const { return handles_; }
  // A drag, as the viewport's handles drive it: begin, any number of
  // moves to a value, then end (commit: one undoable step and a full
  // generation; otherwise the project as it was).
  bool begin_drag(const DimensionHandle& handle);
  void drag_to(double value);
  core::Diagnostics end_drag(bool commit = true);
  [[nodiscard]] bool dragging() const { return drag_.has_value(); }
  // The progress bar and cancel button shown while an evaluation runs.
  [[nodiscard]] QProgressBar& progress_bar() { return *progress_; }
  [[nodiscard]] QAction& cancel_action() { return *cancel_action_; }
  [[nodiscard]] QString status_text() const;

  // Swaps a volume's contents to another kind, touching nothing outside it
  // (a placeholder, empty modelled contents, a procedure, or baked from its
  // procedure's current mesh): one undoable step.
  core::Diagnostics swap_contents(const Id& volume, ContentsKind kind);

  // Shows guidance for a CAD habit on a volume, inline; nullopt (and
  // nothing shown) when the habit is fine there.
  std::optional<Guidance> guide(const Id& volume, Habit habit);
  [[nodiscard]] const std::optional<Guidance>& shown_guidance() const { return guidance_; }
  // The guidance's button.
  void act_on_guidance();
  void dismiss_guidance();

  // --- configurations and design tables (L5, decision 0037)
  // The toolbar's Configuration box: "Authored values", then each
  // configuration of the project; choosing one shows it (one undoable
  // step) and re-evaluates. It follows the history (edits, undo, redo).
  [[nodiscard]] QComboBox& configuration_box() { return *configuration_box_; }
  // Shows `configuration` (null: the authored values), as the box does.
  core::Diagnostics show_configuration(const Id& configuration);
  // Model > Design Table…: every configuration as a grid; OK applies it as
  // one undoable step (set_design_table()).
  DesignTableDialog* open_design_table();

  // --- drawings (L6, decision 0038)
  // The Browser's Drawings section: drawings, their views, their dimensions
  // (drawing_rows(); disposable dimensions marked), with add / remove
  // buttons and a context menu. Every change is one undoable step.
  // The Browser's dock: since U1 the drawings are one of its sections.
  [[nodiscard]] QDockWidget& drawings_dock() { return *drawings_dock_; }
  [[nodiscard]] QTreeWidget& drawings_tree() { return *drawings_; }
  // The drawing window and its sheet; it shows the open drawing, redrawn
  // (draw::draw() on the session's worker) after every evaluation that
  // reaches the screen, preview and full, and every edit of the drawing.
  [[nodiscard]] DrawingWindow& drawing_window() { return *drawing_window_; }
  [[nodiscard]] SheetView& drawing_view();
  // Model > New Drawing: a standard drawing (front, top, right and
  // isometric views, overall dimensions) of the shown evaluation, added
  // as one step and opened. Returns its id (null when refused or nothing
  // is evaluated yet).
  Id new_drawing(const std::string& name = "Sheet");
  // Opens a drawing in the drawing window (false: no such drawing); null
  // closes it.
  bool open_drawing(const Id& drawing);
  [[nodiscard]] const Id& shown_drawing() const;
  // Removes a drawing, a view (with its dimensions) or a dimension, by
  // id, as the dock's Delete does.
  core::Diagnostics delete_drawing_item(const Id& item);
  core::Diagnostics rename_drawing_item(const Id& item, const std::string& name);
  // Dialogs for a new or existing view or dimension of `drawing`; a new
  // dimension goes on `view` (null: the drawing's first view).
  ViewDialog* add_drawing_view(const Id& drawing);
  ViewDialog* edit_drawing_view(const Id& drawing, const Id& view);
  DimensionDialog* add_drawing_dimension(const Id& drawing, const Id& view = {});
  DimensionDialog* edit_drawing_dimension(const Id& drawing, const Id& dimension);
  // Writes `drawing` of what is shown (draw::draw() then write_sheet(),
  // by the extension: .svg, .pdf, .dxf) on the worker; `done` gets the
  // diagnostics on the UI thread. File > Export Drawing… runs the
  // shipping check first, as every export does.
  void export_drawing_to(const Id& drawing, const std::filesystem::path& path,
                         std::function<void(const core::Diagnostics&)> done = {});
  // How many sheets the drawing window has been given (tests wait on it).
  [[nodiscard]] std::uint64_t drawings_shown() const { return drawings_shown_; }
  // Whether the newest redraw hasn't come back from the worker yet.
  [[nodiscard]] bool drawing_pending() const { return drawn_ticket_ < draw_requested_->load(); }

  // --- the context bar (U1, decision 0039)
  [[nodiscard]] ContextBar& context_bar() { return *context_bar_; }
  // The one action of a catalogue tool, shared by the toolbar and menus.
  [[nodiscard]] QAction& tool_action(ToolId id);
  // The context the window is in now (current_context() of what is open).
  [[nodiscard]] Context context() const;
  // What the window has open and selected, as the tool rules see it.
  [[nodiscard]] ToolState tool_state() const;
  // Clicking a tab: enters `tab` from here (leaving a drawing, graph or
  // contents edited in context as needed); false with the reason in the
  // status bar when tab_availability() refuses it.
  bool enter_context(Context tab);
  // Runs a tool as its button does (nothing when it isn't enabled).
  void run_tool(ToolId id);

  // Pick in view: the last pick a feature dialog was given.
  [[nodiscard]] const std::optional<PickResolution>& last_pick() const { return last_pick_; }

 signals:
  // A sheet reached the drawing window (L6).
  void drawingShown();
  // A full (not preview) evaluation reached the screen.
  void evaluationShown();
  // A dimension drag's preview reached the screen (M16).
  void dragPreviewShown();
  // The history moved (an edit, undo or redo).
  void historyChanged();
  // A node preview reached the viewport (M15).
  void nodePreviewShown();

 protected:
  void closeEvent(QCloseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void build_menus();
  // U1: the tool actions and the context bar, with the configuration box
  // and Regenerate on its top row.
  void build_context_bar();
  // Resizes `dock` when `panel` (its widget) folds or unfolds.
  void fold_dock_with_panel(QDockWidget* dock, FoldPanel* panel);
  // Enables tools and tabs and shows the current context (cheap; called
  // after anything that changes what is open or selected).
  void refresh_tools();
  // Asks for a file and imports it into `volume`'s contents.
  void import_contents_dialog(const Id& volume);
  // Sketch mode has its own undo, redo and Esc; the window's shortcuts for
  // those (and F, fit) are set aside meanwhile (H7).
  void suspend_window_shortcuts(bool suspend);
  void build_docks();
  void show_snapshot(const SnapshotPtr& snapshot);
  void refresh_volume_tree();
  void refresh_states();
  void refresh_features();
  void refresh_variables();
  void refresh_inspector();
  void refresh_edit_actions();
  // Fills the Problems list. `started`: the person started what produced
  // them (an open, an edit, Validate, an export...), so the drawer opens
  // when there is anything to show.
  void show_diagnostics(const core::Diagnostics& diagnostics, const QString& source, bool started = false);
  void sync_tree_selection();
  void update_title();
  void add_recent(const std::filesystem::path& path);
  void rebuild_recent_menu();
  void open_dialog_box();
  void save();
  void save_as();
  void export_dialog(ExportFormat format);
  void export_step_dialog();
  void import_dialog();
  // Validates, then runs `go` unless errors stop it (shown in a dialog);
  // warnings ask first. For STEP the weld's errors don't stop it (H5).
  void check_then(const QString& what, ShipTarget target, std::function<void()> go);
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
  QSplitter* split_ = nullptr;  // central_ over the Problems drawer
  ProblemsDrawer* problems_drawer_ = nullptr;
  ProblemsButton* problems_button_ = nullptr;
  QAction* problems_action_ = nullptr;  // View > Problems
  bool remember_layout_ = false;
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
  QAction* save_caches_action_ = nullptr;
  std::optional<ShipCheck> last_check_;
  // The snapshot last_check_ was made from: export_to() reuses its weld
  // only for that same snapshot (H5). Weak, so it keeps nothing alive.
  std::weak_ptr<const Snapshot> last_check_snapshot_;
  QAction* undo_action_ = nullptr;
  QAction* fit_action_ = nullptr;
  std::vector<std::pair<QAction*, QKeySequence>> suspended_shortcuts_;
  std::function<UnsavedChoice()> unsaved_prompt_;
  QAction* redo_action_ = nullptr;
  QAction* regenerate_action_ = nullptr;
  ToolActions* tool_actions_ = nullptr;
  ContextBar* context_bar_ = nullptr;
  QToolBar* context_toolbar_ = nullptr;  // holds the context bar

  Selection selection_;
  bool syncing_ = false;          // updating widgets from selection_
  bool fitted_ = false;           // the view was framed for this project
  core::Diagnostics shown_diagnostics_;  // rows of the diagnostics list
  std::map<core::Id, model::VolumeStatus> last_states_;

  Id editing_volume_;
  std::vector<QTreeWidgetItem*> volume_items_;  // in volume_rows() order (L3: nested)

  QPointer<EditDialog> dialog_;
  std::function<Command()> dialog_command_;
  QTimer* preview_timer_ = nullptr;
  bool previewing_ = false;

  // M15: the node editor, its dock, the node form in the inspector.
  void refresh_graph();
  NodeEditor* node_editor_ = nullptr;
  QDockWidget* graph_dock_ = nullptr;
  NodeForm* node_form_ = nullptr;
  QStackedWidget* inspector_stack_ = nullptr;
  Id graph_volume_;
  std::string preview_port_;
  std::shared_ptr<const NodePreviewResult> last_preview_;

  // M16.
  void refresh_handles();
  void on_handle_dragged(int index, double distance, bool finished);
  void on_part_picked(const PickedPart& part);
  void on_pick_requested(int field);
  std::vector<DimensionHandle> handles_;
  struct Drag {
    DimensionHandle handle;
    double value = 0.0;
    bool moved = false;
  };
  std::optional<Drag> drag_;
  QProgressBar* progress_ = nullptr;
  QPushButton* cancel_button_ = nullptr;
  QToolBar* guidance_bar_ = nullptr;
  QLabel* guidance_label_ = nullptr;
  QAction* guidance_action_ = nullptr;
  std::optional<Guidance> guidance_;
  Id dialog_tree_;  // the tree the open feature dialog edits
  std::optional<PickResolution> last_pick_;

  // L5.
  void refresh_configurations();
  QComboBox* configuration_box_ = nullptr;

  // L6.
  void refresh_drawings();
  // Draws the open drawing as `definition` (null: as the history has it)
  // from `snapshot` (null: the latest) on the worker; a newer request
  // skips an older one still waiting.
  void redraw(SnapshotPtr snapshot = {}, const model::Drawing* definition = nullptr);
  void drawings_context_menu(const QPoint& pos);
  void export_drawing_dialog();
  // The drawing a dock row belongs to, and the row's own id and kind.
  struct DrawingItem {
    int kind = -1;  // DrawingRowKind, -1: no row
    Id id;
    Id drawing;
  };
  [[nodiscard]] DrawingItem current_drawing_item() const;
  QDockWidget* drawings_dock_ = nullptr;  // the Browser's dock (U1)
  Browser* browser_ = nullptr;
  FoldPanel* inspector_panel_ = nullptr;
  QTreeWidget* drawings_ = nullptr;
  DrawingWindow* drawing_window_ = nullptr;
  std::shared_ptr<std::atomic<std::uint64_t>> draw_requested_ = std::make_shared<std::atomic<std::uint64_t>>(0);
  std::uint64_t drawings_shown_ = 0;
  std::uint64_t drawn_ticket_ = 0;  // the newest redraw that came back

  SketchMode* sketch_mode_ = nullptr;
  QPointer<SvgImportDialog> svg_dialog_;
  // U1 phase 3: the sketch's status and constraints in a right-hand dock
  // while sketching, and the left docks folded away meanwhile.
  QDockWidget* sketch_dock_ = nullptr;
  QWidget* sketch_dock_empty_ = nullptr;  // the dock's widget between sketches
  bool sketch_folded_browser_ = false;  // sketch mode folded it; unfold after
  struct SketchSession {
    Id volume;
    feature::Feature feature;
    bool is_new = false;
  };
  std::optional<SketchSession> sketch_session_;
};

}  // namespace vs::app

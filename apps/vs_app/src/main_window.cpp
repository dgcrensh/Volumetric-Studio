#include "main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <fmt/format.h>

#include <algorithm>
#include <fstream>
#include <iterator>

#include "authoring.hpp"
#include "feature_dialog.hpp"
#include "model_dialogs.hpp"
#include "node_editor.hpp"
#include "node_preview.hpp"
#include "reference_choices.hpp"
#include "sketch_mode.hpp"
#include "viewport.hpp"
#include "vs/core/version.hpp"
#include "vs/eval/import.hpp"
#include "vs/proc/bake.hpp"
#include "vs/proc/generator.hpp"
#include "vs/validate/validate.hpp"
#include "widgets.hpp"

namespace vs::app {

namespace {

constexpr int kRecentMax = 8;
constexpr int kRoleKind = Qt::UserRole;      // 0 volume, 1 surface, 3 feature, 4 node
constexpr int kRoleId = Qt::UserRole + 1;    // the subject's id

QString qpath(const std::filesystem::path& p) { return QString::fromStdWString(p.wstring()); }
std::filesystem::path fspath(const QString& s) { return std::filesystem::path(s.toStdWString()); }

QColor qcolor(const Rgb& c) { return QColor::fromRgbF(c.r, c.g, c.b); }

QColor status_color(model::VolumeStatus s) {
  switch (s) {
    case model::VolumeStatus::Current: return {46, 157, 74};
    case model::VolumeStatus::Stale: return {138, 138, 138};
    case model::VolumeStatus::Regenerating: return {224, 160, 32};
    case model::VolumeStatus::Failed: return {208, 48, 48};
  }
  return Qt::black;
}

// A rounded swatch with a letter: the volume tree's kind icons.
QIcon swatch(const QColor& fill, QChar letter, bool hollow = false) {
  QPixmap pm(32, 32);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(fill.darker(150), 2));
  p.setBrush(hollow ? QBrush(Qt::NoBrush) : QBrush(fill));
  p.drawRoundedRect(QRectF(3, 3, 26, 26), 6, 6);
  QFont f = p.font();
  f.setBold(true);
  f.setPixelSize(17);
  p.setFont(f);
  p.setPen(hollow ? fill.darker(150) : QColor(Qt::white));
  p.drawText(QRect(0, 0, 32, 32), Qt::AlignCenter, QString(letter));
  return QIcon(pm);
}

QIcon dot(const QColor& c) {
  QPixmap pm(16, 16);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  p.setBrush(c);
  p.drawEllipse(QRectF(3, 3, 10, 10));
  return QIcon(pm);
}

QIcon kind_icon(const VolumeRow& row, std::size_t index) {
  const QColor c = row.keep_out ? QColor(217, 64, 64) : qcolor(volume_color(index));
  if (row.keep_out) return swatch(c, QChar('K'), true);
  if (row.placeholder) return swatch(c, QChar('?'), true);
  switch (row.kind) {
    case model::VolumeKind::Static: return swatch(c, QChar('S'));
    case model::VolumeKind::Parametric: return swatch(c, QChar('P'));
    case model::VolumeKind::Procedural: return swatch(c, QChar('G'));
  }
  return {};
}

QString severity_text(core::Severity s) {
  switch (s) {
    case core::Severity::Error: return QStringLiteral("Error");
    case core::Severity::Warning: return QStringLiteral("Warning");
    case core::Severity::Info: return QStringLiteral("Info");
  }
  return {};
}

QColor severity_color(core::Severity s) {
  switch (s) {
    case core::Severity::Error: return {208, 48, 48};
    case core::Severity::Warning: return {224, 150, 20};
    case core::Severity::Info: return {110, 120, 135};
  }
  return Qt::black;
}

QTreeWidget* make_tree(const QStringList& headers) {
  auto* tree = new QTreeWidget;
  tree->setHeaderLabels(headers);
  tree->setColumnCount(static_cast<int>(headers.size()));
  tree->setUniformRowHeights(true);
  tree->setAlternatingRowColors(true);
  tree->header()->setStretchLastSection(true);
  return tree;
}

std::optional<Id> row_id(const QTreeWidgetItem* item, int column = 0) {
  if (item == nullptr) return std::nullopt;
  return Id::parse(sstr(item->data(column, kRoleId).toString()));
}

// Values a sketch's dimension expressions can use: the layout's variables,
// or inside a volume its parameters and its contents' variables.
std::map<std::string, double, std::less<>> variable_values(const eval::Evaluation& ev, const Id& volume) {
  std::map<std::string, double, std::less<>> out;
  if (volume.is_null()) return ev.build.layout.variables.values;
  if (const auto* vb = ev.build.find_volume(volume)) out = vb->parameters;
  if (const auto* r = rebuild_of(ev, volume)) {
    for (const auto& [k, v] : r->variables.values) out[k] = v;
  }
  return out;
}

std::string first_error(const core::Diagnostics& d) {
  for (const auto& x : d) {
    if (x.severity == core::Severity::Error) return x.message;
  }
  return d.empty() ? std::string() : d.items().front().message;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  session_ = new Session({}, this);
  session_->set_preparer(&Viewport::prepare);
  viewport_ = new Viewport;
  central_ = new QStackedWidget;
  central_->addWidget(viewport_);
  setCentralWidget(central_);
  build_docks();
  build_menus();

  context_bar_ = new QToolBar(tr("Editing in context"), this);
  context_bar_->setObjectName(QStringLiteral("context"));
  context_bar_->setMovable(false);
  context_label_ = new QLabel;
  context_label_->setStyleSheet(QStringLiteral("QLabel { padding: 2px 8px; font-weight: bold; color: #1d4f91; }"));
  context_bar_->addWidget(context_label_);
  context_bar_->addAction(leave_context_action_);
  addToolBar(Qt::TopToolBarArea, context_bar_);
  context_bar_->setVisible(false);

  // M16: inline guidance for CAD habits, under the context bar.
  addToolBarBreak(Qt::TopToolBarArea);
  guidance_bar_ = new QToolBar(tr("Guidance"), this);
  guidance_bar_->setObjectName(QStringLiteral("guidance"));
  guidance_bar_->setMovable(false);
  guidance_label_ = new QLabel;
  guidance_label_->setWordWrap(true);
  guidance_label_->setStyleSheet(QStringLiteral("QLabel { padding: 4px 8px; color: #5a3d00; }"));
  guidance_label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  guidance_bar_->setStyleSheet(QStringLiteral("QToolBar { background: #fff4d6; border-bottom: 1px solid #e0c070; }"));
  guidance_bar_->addWidget(guidance_label_);
  guidance_action_ = guidance_bar_->addAction(QString(), this, &MainWindow::act_on_guidance);
  guidance_bar_->addAction(tr("Dismiss"), this, &MainWindow::dismiss_guidance);
  addToolBar(Qt::TopToolBarArea, guidance_bar_);
  guidance_bar_->setVisible(false);

  status_label_ = new QLabel(tr("No project"));
  frame_label_ = new QLabel;
  statusBar()->addWidget(status_label_, 1);
  // M16: progress and cancel for long generations.
  progress_ = new QProgressBar;
  progress_->setMaximumWidth(220);
  progress_->setTextVisible(true);
  progress_->setVisible(false);
  cancel_button_ = new QPushButton(tr("Cancel"));
  cancel_button_->setVisible(false);
  connect(cancel_button_, &QPushButton::clicked, cancel_action_, &QAction::trigger);
  statusBar()->addPermanentWidget(progress_);
  statusBar()->addPermanentWidget(cancel_button_);
  statusBar()->addPermanentWidget(frame_label_);

  state_timer_ = new QTimer(this);
  state_timer_->setInterval(100);
  connect(state_timer_, &QTimer::timeout, this, &MainWindow::refresh_states);
  preview_timer_ = new QTimer(this);
  preview_timer_->setSingleShot(true);
  preview_timer_->setInterval(150);
  connect(preview_timer_, &QTimer::timeout, this, &MainWindow::run_preview);

  connect(session_, &Session::evaluated, this, &MainWindow::show_snapshot);
  connect(session_, &Session::evaluationStarted, this, [this] {
    status_label_->setText(tr("Regenerating…"));
    cancel_action_->setEnabled(true);
    state_timer_->start();
    progress_->setRange(0, 0);  // busy until the first volume reports
    progress_->setFormat(tr("Regenerating"));
    progress_->setVisible(true);
    cancel_button_->setVisible(true);
  });
  connect(session_, &Session::progressed, this, [this](int done, int total) {
    status_label_->setText(tr("Regenerating… %1 of %2 volumes").arg(done).arg(total));
    if (total > 0) {
      progress_->setRange(0, total);
      progress_->setValue(done);
      progress_->setFormat(tr("%v of %m volumes"));
    }
  });
  connect(viewport_, &Viewport::handleDragged, this, &MainWindow::on_handle_dragged);
  connect(viewport_, &Viewport::partPicked, this, &MainWindow::on_part_picked);
  connect(session_, &Session::projectChanged, this, [this] {
    // A new project (opened, or set by a script): a fresh history.
    close_dialog();
    if (sketch_mode_ != nullptr) finish_sketch(false);
    history_.reset(session_->project());
    editing_volume_ = {};
    graph_volume_ = {};
    node_editor_->clear();
    clear_node_preview();
    viewport_->set_context({}, {});
    context_bar_->setVisible(false);
    drag_.reset();
    dismiss_guidance();
    fitted_ = false;
    selection_ = {};
    refresh_edit_actions();
    update_title();
  });
  connect(viewport_, &Viewport::picked, this, [this](core::Id subject) {
    const SnapshotPtr s = session_->latest();
    if (subject.is_null() || !s) {
      select(Selection::none());
    } else if (s->evaluation.document.find_connection(subject) != nullptr) {
      select(Selection::of_surface(subject));
    } else if (s->evaluation.document.find_volume(subject) != nullptr) {
      select(Selection::of_volume(subject));
    } else if (s->evaluation.document.layout.find(subject) != nullptr) {
      select(Selection::of_feature(subject));
    } else {
      select(Selection::none());
    }
  });
  connect(session_, &Session::nodePreviewed, this,
          [this](quint64, std::shared_ptr<const NodePreviewResult> result, std::shared_ptr<const PreparedScene> prepared) {
            // A preview for a graph no longer open, or a node since deleted.
            if (result->volume != graph_volume_ || node_editor_->graph().find_node(result->node) == nullptr) return;
            last_preview_ = result;
            viewport_->set_overlay(result->scene, std::move(prepared), result->volume);
            node_editor_->show_preview(*result);
            emit nodePreviewShown();
          });
  connect(viewport_, &Viewport::frameRendered, this, [this](double ms) {
    frame_label_->setText(tr("frame %1 ms").arg(ms, 0, 'f', 1));
  });

  resize(1440, 900);
  refresh_edit_actions();
  update_title();
}

MainWindow::~MainWindow() {
  if (dialog_) dialog_->disconnect(this);
}

// --- menus and docks -----------------------------------------------------------------------

void MainWindow::build_menus() {
  QMenu* file = menuBar()->addMenu(tr("&File"));
  file->addAction(tr("&New"), QKeySequence::New, this, [this] { new_project(); });
  file->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::open_dialog_box);
  recent_menu_ = file->addMenu(tr("Open &Recent"));
  rebuild_recent_menu();
  file->addSeparator();
  file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::save);
  file->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, &MainWindow::save_as);
  save_caches_action_ = file->addAction(tr("Save &Caches With Project"));
  save_caches_action_->setCheckable(true);
  save_caches_action_->setToolTip(tr("Store generated contents in the project, so reopening it skips "
                                     "regenerating what hasn't changed"));
  save_caches_action_->setChecked(
      QSettings(QStringLiteral("Volumetric Studio"), QStringLiteral("vs_app"))
          .value(QStringLiteral("save_caches"), true)
          .toBool());
  connect(save_caches_action_, &QAction::toggled, this, [](bool on) {
    QSettings(QStringLiteral("Volumetric Studio"), QStringLiteral("vs_app"))
        .setValue(QStringLiteral("save_caches"), on);
  });
  file->addSeparator();
  file->addAction(tr("&Import as New Volume…"), QKeySequence(Qt::CTRL | Qt::Key_I), this,
                  &MainWindow::import_dialog);
  file->addSeparator();
  file->addAction(tr("Export S&TEP…"), this, &MainWindow::export_step_dialog);
  file->addAction(tr("Export &STL…"), this, [this] { export_dialog(ExportFormat::Stl); });
  file->addAction(tr("Export &3MF…"), this, [this] { export_dialog(ExportFormat::ThreeMf); });
  file->addSeparator();
  file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

  QMenu* edit = menuBar()->addMenu(tr("&Edit"));
  undo_action_ = edit->addAction(tr("&Undo"), QKeySequence::Undo, this, &MainWindow::undo);
  redo_action_ = edit->addAction(tr("&Redo"), QKeySequence::Redo, this, &MainWindow::redo);

  QMenu* insert = menuBar()->addMenu(tr("&Insert"));
  using K = feature::FeatureKind;
  auto add_kind = [&](K kind, const QKeySequence& key = {}) {
    QAction* a = insert->addAction(qstr(kind_title(kind)) + QStringLiteral("…"), this,
                                   [this, kind] { insert_feature(kind); });
    if (!key.isEmpty()) a->setShortcut(key);
  };
  add_kind(K::Sketch, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
  insert->addSeparator();
  add_kind(K::DatumPlane);
  add_kind(K::DatumAxis);
  add_kind(K::DatumPoint);
  insert->addSeparator();
  add_kind(K::Extrude, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
  add_kind(K::Revolve);
  add_kind(K::Sweep);
  add_kind(K::Loft);
  insert->addSeparator();
  add_kind(K::Shell);
  add_kind(K::Fillet);
  add_kind(K::Chamfer);
  insert->addSeparator();
  add_kind(K::LinearPattern);
  add_kind(K::CircularPattern);

  QMenu* volume = menuBar()->addMenu(tr("&Volume"));
  volume->addAction(tr("New &volume…"), this, [this] { new_volume(); });
  volume->addAction(tr("New &connection surface…"), this, [this] { new_connection(); });
  volume->addAction(tr("New &keep-out region…"), this, [this] { new_keep_out(); });
  volume->addAction(tr("New &link…"), this, [this] { new_link(); });
  volume->addSeparator();
  volume->addAction(tr("&Edit contents in context"), QKeySequence(Qt::CTRL | Qt::Key_E), this, [this] {
    if (selection_.kind == Selection::Kind::Volume) edit_in_context(selection_.id);
  });
  leave_context_action_ = volume->addAction(tr("&Done editing contents"), this, &MainWindow::leave_context);
  leave_context_action_->setEnabled(false);
  volume->addSeparator();
  volume->addAction(tr("Edit &graph"), QKeySequence(Qt::CTRL | Qt::Key_G), this, [this] {
    if (selection_.kind == Selection::Kind::Volume || selection_.kind == Selection::Kind::Node) {
      edit_graph(selection_.volume.is_null() ? selection_.id : selection_.volume);
    }
  });
  volume->addAction(tr("Attach &template…"), this, [this] {
    const Id v = selection_.kind == Selection::Kind::Volume ? selection_.id : graph_volume_;
    if (!v.is_null()) attach_template(v);
  });
  volume->addAction(tr("&Bake"), this, [this] {
    const Id v = selection_.kind == Selection::Kind::Volume ? selection_.id : graph_volume_;
    if (!v.is_null()) bake(v);
  });
  volume->addAction(tr("&Unbake"), this, [this] {
    if (selection_.kind == Selection::Kind::Volume) unbake(selection_.id);
  });

  QMenu* model = menuBar()->addMenu(tr("&Model"));
  model->addAction(tr("&Regenerate"), QKeySequence(Qt::Key_F5), session_, &Session::regenerate);
  cancel_action_ = model->addAction(tr("&Cancel"), QKeySequence(Qt::Key_Escape), session_, &Session::cancel);
  cancel_action_->setEnabled(false);
  model->addAction(tr("&Validate"), QKeySequence(Qt::CTRL | Qt::Key_R), this, [this] { validate(); });

  QMenu* view = menuBar()->addMenu(tr("V&iew"));
  view->addAction(tr("&Fit All"), QKeySequence(Qt::Key_F), viewport_, &Viewport::fit_all);
  view->addSeparator();
  const std::pair<const char*, StandardView> views[] = {
      {"&Isometric", StandardView::Iso}, {"&Top", StandardView::Top},   {"&Bottom", StandardView::Bottom},
      {"F&ront", StandardView::Front},   {"Bac&k", StandardView::Back}, {"&Left", StandardView::Left},
      {"R&ight", StandardView::Right}};
  for (const auto& [label, v] : views) {
    const StandardView which = v;
    view->addAction(tr(label), this, [this, which] { viewport_->set_view(which); });
  }
  view->addSeparator();
  for (QDockWidget* dock : findChildren<QDockWidget*>()) view->addAction(dock->toggleViewAction());

  QMenu* help = menuBar()->addMenu(tr("&Help"));
  help->addAction(tr("&About"), this, [this] {
    const auto v = core::version_string();
    QMessageBox::about(this, tr("Volumetric CAD Studio"),
                       tr("Volumetric CAD Studio %1\n\nEvery design is a tree of volumes.").arg(qstr(v)));
  });
}

void MainWindow::build_docks() {
  setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks);
  auto dock = [this](const QString& title, const QString& name, QWidget* w, Qt::DockWidgetArea area) {
    auto* d = new QDockWidget(title, this);
    d->setObjectName(name);
    d->setWidget(w);
    addDockWidget(area, d);
    return d;
  };

  volumes_ = make_tree({tr("Name"), tr("Kind"), tr("Status")});
  volumes_->setIconSize(QSize(18, 18));
  volumes_->setIndentation(14);
  volumes_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  volumes_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  volumes_->setMinimumWidth(360);
  volumes_->setContextMenuPolicy(Qt::CustomContextMenu);
  dock(tr("Volumes"), "volumes", volumes_, Qt::LeftDockWidgetArea);
  connect(volumes_, &QTreeWidget::itemSelectionChanged, this, [this] {
    if (syncing_) return;
    const auto items = volumes_->selectedItems();
    if (items.isEmpty()) return select(Selection::none());
    const auto id = row_id(items.front());
    if (!id) return select(Selection::none());
    select(items.front()->data(0, kRoleKind).toInt() == 1 ? Selection::of_surface(*id) : Selection::of_volume(*id));
  });
  connect(volumes_, &QTreeWidget::customContextMenuRequested, this, &MainWindow::volume_context_menu);
  connect(volumes_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
    const auto id = row_id(item);
    if (!id) return;
    if (item->data(0, kRoleKind).toInt() == 1) {
      edit_connection(*id);
    } else {
      edit_volume(*id);
    }
  });

  features_ = make_tree({tr("Feature"), tr("Kind"), tr("Status")});
  features_->setRootIsDecorated(false);
  features_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  features_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  features_->setContextMenuPolicy(Qt::CustomContextMenu);
  dock(tr("Features"), "features", features_, Qt::LeftDockWidgetArea);
  connect(features_, &QTreeWidget::itemSelectionChanged, this, [this] {
    if (syncing_) return;
    const auto items = features_->selectedItems();
    if (items.isEmpty()) return;
    const auto id = row_id(items.front());
    const auto volume = row_id(items.front(), 1);
    if (!id) return;
    const bool node = items.front()->data(0, kRoleKind).toInt() == 4;
    const core::Id v = volume ? *volume : core::Id{};
    select(node ? Selection::of_node(*id, v) : Selection::of_feature(*id, v));
  });
  connect(features_, &QTreeWidget::customContextMenuRequested, this, &MainWindow::feature_context_menu);
  connect(features_, &QTreeWidget::itemDoubleClicked, this,
          [this](QTreeWidgetItem* item, int) { activate_feature_row(item); });

  // Variables of the tree the feature panel shows.
  auto* vars = new QWidget;
  auto* vl = new QVBoxLayout(vars);
  vl->setContentsMargins(0, 0, 0, 0);
  variables_ = new QTableWidget(0, 3);
  variables_->setHorizontalHeaderLabels({tr("Name"), tr("Expression"), tr("Value")});
  variables_->horizontalHeader()->setStretchLastSection(true);
  variables_->verticalHeader()->setVisible(false);
  vl->addWidget(variables_);
  auto* vb = new QHBoxLayout;
  auto* add_var = new QPushButton(tr("Add variable"));
  auto* remove_var = new QPushButton(tr("Delete"));
  vb->addWidget(add_var);
  vb->addWidget(remove_var);
  vb->addStretch(1);
  vl->addLayout(vb);
  dock(tr("Variables"), "variables", vars, Qt::LeftDockWidgetArea);
  connect(add_var, &QPushButton::clicked, this, [this] {
    const Id volume = shown_tree();
    const auto* tree = tree_of(history_.project().document, volume);
    if (tree == nullptr) return;
    int n = 1;
    while (tree->variables().find(fmt::format("var{}", n)) != nullptr) ++n;
    apply(add_variable(volume, fmt::format("var{}", n), "0"));
  });
  connect(remove_var, &QPushButton::clicked, this, [this] {
    const int row = variables_->currentRow();
    if (row < 0) return;
    const auto id = Id::parse(sstr(variables_->item(row, 0)->data(Qt::UserRole).toString()));
    if (id) apply(remove_variable(shown_tree(), *id));
  });
  connect(variables_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
    if (syncing_ || item->column() > 1) return;
    const int row = item->row();
    const auto id = Id::parse(sstr(variables_->item(row, 0)->data(Qt::UserRole).toString()));
    if (!id) return;
    const auto d = apply(set_variable(shown_tree(), *id, sstr(variables_->item(row, 0)->text().trimmed()),
                                      sstr(variables_->item(row, 1)->text().trimmed())));
    if (d.has_errors()) refresh_variables();  // put the cell back
  });

  inspector_ = make_tree({tr("Property"), tr("Value")});
  inspector_->setAlternatingRowColors(false);
  inspector_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  inspector_->setIndentation(12);
  inspector_->setMinimumWidth(300);
  // A selected node of the graph being edited shows its settings form
  // instead (M15: "parameter editing in the inspector").
  inspector_stack_ = new QStackedWidget;
  inspector_stack_->addWidget(inspector_);
  node_form_ = new NodeForm;
  inspector_stack_->addWidget(node_form_);
  dock(tr("Inspector"), "inspector", inspector_stack_, Qt::RightDockWidgetArea);
  connect(node_form_, &NodeForm::settingEdited, this, [this](core::Id node, const QString& key, const QString& text) {
    node_editor_->set_setting_text(node, sstr(key), text);
  });
  connect(node_form_, &NodeForm::renamed, this, [this](core::Id node, const QString& name) {
    const proc::Node* n = node_editor_->graph().find_node(node);
    if (n != nullptr && qstr(n->name) != name) node_editor_->rename_node(node, sstr(name));
  });
  connect(node_form_, &NodeForm::exposeRequested, this, [this](core::Id node, const QString& port) {
    node_editor_->expose_input(node, sstr(port));
  });

  diagnostics_ = make_tree({tr("Severity"), tr("Code"), tr("Message"), tr("Act on")});
  diagnostics_->setRootIsDecorated(false);
  diagnostics_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  diagnostics_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  diagnostics_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  diagnostics_->header()->setStretchLastSection(false);
  QDockWidget* diagnostics_dock = dock(tr("Diagnostics"), "diagnostics", diagnostics_, Qt::BottomDockWidgetArea);
  connect(diagnostics_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
    activate_diagnostic(diagnostics_->indexOfTopLevelItem(item));
  });

  // The node editor (M15), tabbed with the diagnostics.
  node_editor_ = new NodeEditor;
  node_editor_->set_apply([this](const Command& c) { return apply(c); });
  graph_dock_ = dock(tr("Graph"), "graph", node_editor_, Qt::BottomDockWidgetArea);
  tabifyDockWidget(diagnostics_dock, graph_dock_);
  diagnostics_dock->raise();
  connect(node_editor_, &NodeEditor::nodeSelected, this, [this](core::Id volume, core::Id node) {
    if (syncing_) return;
    select(node.is_null() ? Selection::of_volume(volume) : Selection::of_node(node, volume));
  });
  connect(node_editor_, &NodeEditor::previewRequested, this,
          [this](core::Id node, const QString& port) { preview_node(node, sstr(port)); });
  connect(node_editor_, &NodeEditor::previewOptionsChanged, this, [this] {
    if (!node_editor_->previewed_node().is_null()) preview_node(node_editor_->previewed_node(), preview_port_);
  });
  connect(node_editor_, &NodeEditor::templatesRequested, this, [this] {
    if (!graph_volume_.is_null()) attach_template(graph_volume_);
  });
  connect(node_editor_, &NodeEditor::bakeRequested, this, [this] {
    if (!graph_volume_.is_null()) bake(graph_volume_);
  });
  connect(node_editor_, &NodeEditor::closeRequested, this, &MainWindow::leave_graph);
}

// --- projects ---------------------------------------------------------------------------

bool MainWindow::open_project(const std::filesystem::path& path) {
  const core::Diagnostics d = session_->open(path);
  if (d.has_errors()) {
    show_diagnostics(d, tr("Open"));
    status_label_->setText(tr("Can't open %1").arg(qpath(path)));
    return false;
  }
  add_recent(path);
  return true;
}

void MainWindow::new_project(const std::string& name) {
  model::Project project;
  project.document = model::make_document(name);
  session_->set_project(std::move(project));
}

void MainWindow::show_project() {
  session_->update_project(history_.current());
  previewing_ = false;
  refresh_edit_actions();
  update_title();
  refresh_graph();
  emit historyChanged();
}

// The node editor follows the History: after every edit, undo and redo it
// shows the graph as it now stands, and the previewed node is previewed
// again (cheap: unchanged nodes come from the node cache).
void MainWindow::refresh_graph() {
  if (graph_volume_.is_null()) return;
  const SnapshotPtr s = session_->latest();
  if (!graph_of(history_.project().document, graph_volume_)) {
    // Undone back to a placeholder, or baked: nothing to edit any more.
    leave_graph();
    return;
  }
  node_editor_->show_graph(history_.project(), s ? &s->evaluation : nullptr, graph_volume_);
  if (selection_.kind == Selection::Kind::Node && selection_.volume == graph_volume_) {
    if (node_editor_->graph().find_node(selection_.id) == nullptr) {
      select(Selection::of_volume(graph_volume_));
    } else {
      node_editor_->select_node(selection_.id);
      refresh_inspector();
    }
  }
  const Id previewed = node_editor_->previewed_node();
  if (!previewed.is_null()) {
    if (node_editor_->graph().find_node(previewed) != nullptr) {
      preview_node(previewed, preview_port_);
    } else {
      clear_node_preview();
    }
  }
}

core::Diagnostics MainWindow::apply(const Command& command) {
  const core::Diagnostics d = history_.apply(command);
  if (d.has_errors()) {
    show_diagnostics(d, tr("Refused edit"));
    status_label_->setText(tr("Can't %1: %2").arg(qstr(command.label).toLower(), qstr(first_error(d))));
    // A preview may be on screen: put the project back.
    if (previewing_) session_->update_project(history_.current());
    previewing_ = false;
    return d;
  }
  show_project();
  status_label_->setText(qstr(command.label));
  return d;
}

void MainWindow::undo() {
  if (sketch_mode_ != nullptr) return sketch_mode_->undo();
  close_dialog();
  if (history_.undo()) {
    status_label_->setText(tr("Undid: %1").arg(qstr(history_.redo_label())));
    show_project();
  }
}

void MainWindow::redo() {
  if (sketch_mode_ != nullptr) return sketch_mode_->redo();
  close_dialog();
  if (history_.redo()) {
    status_label_->setText(tr("Redid: %1").arg(qstr(history_.undo_label())));
    show_project();
  }
}

void MainWindow::refresh_edit_actions() {
  if (undo_action_ == nullptr) return;
  undo_action_->setEnabled(history_.can_undo());
  redo_action_->setEnabled(history_.can_redo());
  undo_action_->setText(history_.can_undo() ? tr("&Undo %1").arg(qstr(history_.undo_label())) : tr("&Undo"));
  redo_action_->setText(history_.can_redo() ? tr("&Redo %1").arg(qstr(history_.redo_label())) : tr("&Redo"));
}

// --- selection and panels -------------------------------------------------------------------

std::vector<Id> MainWindow::highlight_for(const Selection& s) const {
  switch (s.kind) {
    case Selection::Kind::None: return {};
    case Selection::Kind::Volume:
    case Selection::Kind::ConnectionSurface: return {s.id};
    case Selection::Kind::Feature:
      if (s.volume.is_null()) return {s.id};
      return {s.volume, s.id};
    // A node is selected on the canvas, and previewed in place: the
    // volume's highlight would hide the preview.
    case Selection::Kind::Node: return {};
  }
  return {};
}

Id MainWindow::shown_tree() const {
  if (!editing_volume_.is_null()) return editing_volume_;
  if (selection_.kind == Selection::Kind::Volume) return selection_.id;
  if (selection_.kind == Selection::Kind::Feature || selection_.kind == Selection::Kind::Node) return selection_.volume;
  return {};
}

void MainWindow::select(const Selection& selection) {
  selection_ = selection;
  viewport_->set_highlight(highlight_for(selection));
  sync_tree_selection();
  refresh_features();
  refresh_variables();
  refresh_inspector();
  // M15: a node of the graph being edited is selected on the canvas and
  // previewed in the viewport; anything else ends the preview.
  if (selection.kind == Selection::Kind::Node && selection.volume == graph_volume_ && !graph_volume_.is_null()) {
    syncing_ = true;
    if (node_editor_->selected_node() != selection.id) node_editor_->select_node(selection.id);
    syncing_ = false;
    if (node_editor_->previewed_node() != selection.id) preview_port_.clear();
    preview_node(selection.id, preview_port_);
  } else if (selection.kind != Selection::Kind::Node) {
    clear_node_preview();
  }
  refresh_handles();
}

void MainWindow::show_snapshot(const SnapshotPtr& snapshot) {
  const bool fit = !fitted_ && !snapshot->scene.items.empty();
  viewport_->show_scene(snapshot->scene, snapshot->prepared, fit);
  fitted_ = fitted_ || fit;
  if (snapshot->drag) {
    // A dimension drag's preview: the viewport only; the panels follow
    // when the drag ends.
    if (drag_) {
      status_label_->setText(tr("Dragging %1: %2 mm (preview: %3 ms)")
                                 .arg(qstr(drag_->handle.label))
                                 .arg(drag_->value, 0, 'g', 6)
                                 .arg(snapshot->evaluate_ms + snapshot->scene_ms, 0, 'f', 0));
    }
    emit dragPreviewShown();
    return;
  }
  // Keep the selection if it still exists.
  const auto& doc = snapshot->evaluation.document;
  if ((selection_.kind == Selection::Kind::Volume && doc.find_volume(selection_.id) == nullptr) ||
      (selection_.kind == Selection::Kind::ConnectionSurface && doc.find_connection(selection_.id) == nullptr) ||
      (selection_.kind == Selection::Kind::Feature && tree_of(doc, selection_.volume) != nullptr &&
       tree_of(doc, selection_.volume)->find(selection_.id) == nullptr)) {
    selection_ = {};
  }
  if (!editing_volume_.is_null()) {
    if (doc.find_volume(editing_volume_) == nullptr || tree_of(doc, editing_volume_) == nullptr) {
      leave_context();
    } else {
      std::vector<Id> inputs;
      for (const auto& s : doc.connections) {
        if (s.joins(editing_volume_)) inputs.push_back(s.id);
      }
      viewport_->set_context(editing_volume_, inputs);
    }
  }
  refresh_volume_tree();
  refresh_features();
  refresh_variables();
  refresh_inspector();
  viewport_->set_highlight(highlight_for(selection_));
  if (!graph_volume_.is_null()) {
    // New marks on the canvas (failed or infeasible nodes).
    if (graph_of(history_.project().document, graph_volume_)) {
      node_editor_->show_graph(history_.project(), &snapshot->evaluation, graph_volume_);
    }
  }
  core::Diagnostics all = snapshot->evaluation.diagnostics;
  all.append(snapshot->scene.diagnostics);
  show_diagnostics(all, snapshot->preview ? tr("Preview") : tr("Evaluation"));
  if (snapshot->preview) {
    status_label_->setText(tr("Regenerating…"));
    return;
  }
  state_timer_->stop();
  last_states_.clear();
  cancel_action_->setEnabled(false);
  progress_->setVisible(false);
  cancel_button_->setVisible(false);
  refresh_handles();
  const auto& st = snapshot->evaluation.stats;
  status_label_->setText(tr("%1 — %2 volumes, %3 regenerated, %4 from cache, %5 ms (display %6 ms)%7%8")
                             .arg(qstr(doc.name))
                             .arg(doc.volumes.size())
                             .arg(st.regenerated)
                             .arg(st.cache_hits)
                             .arg(snapshot->evaluate_ms, 0, 'f', 0)
                             .arg(snapshot->scene_ms, 0, 'f', 0)
                             .arg(snapshot->evaluation.cancelled ? tr(" — cancelled") : QString())
                             .arg(previewing_ ? tr(" — preview") : QString()));
  emit evaluationShown();
}

void MainWindow::refresh_volume_tree() {
  const SnapshotPtr s = session_->latest();
  syncing_ = true;
  volumes_->clear();
  if (s) {
    const auto& ev = s->evaluation;
    auto* vols = new QTreeWidgetItem(volumes_, {tr("Volumes")});
    vols->setFlags(Qt::ItemIsEnabled);
    const auto rows = volume_rows(ev, last_states_);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const VolumeRow& r = rows[i];
      auto* item = new QTreeWidgetItem(vols);
      item->setText(0, qstr(r.name));
      item->setIcon(0, kind_icon(r, i));
      item->setText(1, qstr(kind_label(r)));
      QString status = qstr(status_label(r.status));
      if (r.stale_contents) status += tr(" (showing stale)");
      if (r.errors > 0) status += tr(" — %n error(s)", nullptr, static_cast<int>(r.errors));
      else if (r.warnings > 0) status += tr(" — %n warning(s)", nullptr, static_cast<int>(r.warnings));
      item->setText(2, status);
      item->setIcon(2, dot(status_color(r.status)));
      item->setData(0, kRoleKind, 0);
      item->setData(0, kRoleId, qstr(r.id.to_string()));
      if (r.id == editing_volume_) {
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
      } else if (!editing_volume_.is_null()) {
        for (int c = 0; c < 3; ++c) item->setForeground(c, QColor(150, 150, 155));
      }
    }
    const auto surfaces = surface_rows(ev);
    if (!surfaces.empty()) {
      auto* group = new QTreeWidgetItem(volumes_, {tr("Connection surfaces")});
      group->setFlags(Qt::ItemIsEnabled);
      for (const SurfaceRow& r : surfaces) {
        auto* item = new QTreeWidgetItem(group);
        item->setText(0, qstr(r.name));
        item->setIcon(0, dot(qcolor(connection_color(r.type))));
        item->setText(1, qstr(model::to_string(r.type)));
        QString joins;
        for (const auto& v : r.volumes) joins += (joins.isEmpty() ? "" : " + ") + qstr(v);
        item->setText(2, r.ok ? joins : tr("patch failed"));
        item->setData(0, kRoleKind, 1);
        item->setData(0, kRoleId, qstr(r.id.to_string()));
      }
    }
    volumes_->expandAll();
  }
  syncing_ = false;
  sync_tree_selection();
}

void MainWindow::refresh_states() {
  const SnapshotPtr s = session_->latest();
  if (!s) return;
  auto states = session_->states();
  if (states == last_states_) return;
  last_states_ = std::move(states);
  const auto rows = volume_rows(s->evaluation, last_states_);
  QTreeWidgetItem* vols = volumes_->topLevelItemCount() > 0 ? volumes_->topLevelItem(0) : nullptr;
  if (vols == nullptr) return;
  for (int i = 0; i < vols->childCount() && i < static_cast<int>(rows.size()); ++i) {
    const auto& r = rows[static_cast<std::size_t>(i)];
    vols->child(i)->setText(2, qstr(status_label(r.status)));
    vols->child(i)->setIcon(2, dot(status_color(r.status)));
  }
}

void MainWindow::refresh_features() {
  syncing_ = true;
  features_->clear();
  const SnapshotPtr s = session_->latest();
  if (s) {
    const Id volume = shown_tree();
    const auto* v = s->evaluation.document.find_volume(volume);
    QString title = v != nullptr ? tr("%1 — features").arg(qstr(v->name)) : tr("Layout features");
    if (!editing_volume_.is_null()) title = tr("%1 — contents (in context)").arg(v != nullptr ? qstr(v->name) : QString());
    features_->setHeaderLabels({title, tr("Kind"), tr("Status")});
    for (const FeatureRow& row : feature_rows(s->evaluation, volume)) {
      auto* item = new QTreeWidgetItem(features_);
      item->setText(0, qstr(row.name));
      item->setText(1, qstr(row.kind));
      item->setText(2, qstr(row.status));
      if (row.status == "failed" || row.status == "infeasible") item->setForeground(2, QColor(208, 48, 48));
      if (row.status == "suppressed") {
        for (int c = 0; c < 3; ++c) item->setForeground(c, QColor(150, 150, 155));
      }
      item->setData(0, kRoleKind, row.selection.kind == Selection::Kind::Node ? 4 : 3);
      item->setData(0, kRoleId, qstr(row.selection.id.to_string()));
      item->setData(1, kRoleId, qstr(row.selection.volume.to_string()));
      if (row.selection == selection_) item->setSelected(true);
    }
  }
  syncing_ = false;
}

void MainWindow::refresh_variables() {
  syncing_ = true;
  variables_->setRowCount(0);
  const SnapshotPtr s = session_->latest();
  const Id volume = shown_tree();
  const auto* tree = tree_of(history_.project().document, volume);
  if (tree != nullptr) {
    std::map<std::string, double, std::less<>> values;
    if (s) {
      if (const auto* r = rebuild_of(s->evaluation, volume)) values = r->variables.values;
    }
    for (const auto& var : tree->variables().variables()) {
      const int row = variables_->rowCount();
      variables_->insertRow(row);
      auto* name = new QTableWidgetItem(qstr(var.name));
      name->setData(Qt::UserRole, qstr(var.id.to_string()));
      variables_->setItem(row, 0, name);
      variables_->setItem(row, 1, new QTableWidgetItem(qstr(var.expression)));
      const auto it = values.find(var.name);
      auto* value = new QTableWidgetItem(it != values.end() ? QString::number(it->second, 'g', 10) : tr("?"));
      value->setFlags(value->flags() & ~Qt::ItemIsEditable);
      value->setForeground(QColor(90, 90, 100));
      variables_->setItem(row, 2, value);
    }
  }
  variables_->setEnabled(tree != nullptr);
  syncing_ = false;
}

void MainWindow::refresh_inspector() {
  const bool node = selection_.kind == Selection::Kind::Node && !graph_volume_.is_null() &&
                    selection_.volume == graph_volume_ && node_editor_->graph().find_node(selection_.id) != nullptr;
  if (node) {
    node_form_->show_node(node_editor_->graph(), selection_.id, node_editor_->context());
    inspector_stack_->setCurrentWidget(node_form_);
  } else {
    node_form_->show_node({}, {}, {});
    inspector_stack_->setCurrentWidget(inspector_);
  }
  inspector_->clear();
  const SnapshotPtr s = session_->latest();
  if (!s) return;
  std::map<std::string, QTreeWidgetItem*> groups;
  for (const Property& p : inspect(s->evaluation, selection_)) {
    QTreeWidgetItem*& group = groups[p.group];
    if (group == nullptr) {
      group = new QTreeWidgetItem(inspector_, {qstr(p.group)});
      QFont f = group->font(0);
      f.setBold(true);
      group->setFont(0, f);
      group->setFirstColumnSpanned(true);
    }
    auto* item = new QTreeWidgetItem(group, {qstr(p.name), qstr(p.value)});
    item->setToolTip(1, qstr(p.value));
  }
  inspector_->expandAll();
}

void MainWindow::show_diagnostics(const core::Diagnostics& diagnostics, const QString& source) {
  shown_diagnostics_ = diagnostics;
  diagnostics_->clear();
  for (const core::Diagnostic& d : diagnostics) {
    auto* item = new QTreeWidgetItem(diagnostics_);
    item->setText(0, severity_text(d.severity));
    item->setIcon(0, dot(severity_color(d.severity)));
    item->setText(1, qstr(d.code));
    item->setText(2, qstr(d.message));
    item->setToolTip(2, qstr(d.to_string()));
    if (const auto* t = d.target()) item->setText(3, qstr(t->label));
    if (d.severity == core::Severity::Info) {
      for (int c = 0; c < 4; ++c) item->setForeground(c, QColor(110, 120, 135));
    }
  }
  const auto errors = diagnostics.count(core::Severity::Error);
  const auto warnings = diagnostics.count(core::Severity::Warning);
  if (auto* dock = qobject_cast<QDockWidget*>(diagnostics_->parentWidget())) {
    dock->setWindowTitle(tr("Diagnostics — %1: %2 errors, %3 warnings").arg(source).arg(errors).arg(warnings));
  }
}

void MainWindow::activate_diagnostic(int row) {
  const SnapshotPtr s = session_->latest();
  if (!s || row < 0 || static_cast<std::size_t>(row) >= shown_diagnostics_.size()) return;
  const core::Diagnostic& d = shown_diagnostics_[static_cast<std::size_t>(row)];
  const Selection target = selection_for(d, s->evaluation.document);
  select(target);
  if (target.empty()) status_label_->setText(tr("%1 acts on the layout as a whole").arg(qstr(d.code)));
}

void MainWindow::sync_tree_selection() {
  syncing_ = true;
  volumes_->clearSelection();
  const core::Id subject = viewport_subject(selection_);
  if (!subject.is_null()) {
    const QString id = qstr(subject.to_string());
    for (int g = 0; g < volumes_->topLevelItemCount(); ++g) {
      QTreeWidgetItem* group = volumes_->topLevelItem(g);
      for (int i = 0; i < group->childCount(); ++i) {
        if (group->child(i)->data(0, kRoleId).toString() == id) {
          group->child(i)->setSelected(true);
          volumes_->scrollToItem(group->child(i));
        }
      }
    }
  }
  syncing_ = false;
}

void MainWindow::update_title() {
  QString title = tr("Volumetric CAD Studio");
  if (session_->has_project()) {
    title = qstr(history_.project().document.name) + (history_.dirty() ? QStringLiteral(" *") : QString()) +
            QStringLiteral(" — ") + title;
  }
  setWindowTitle(title);
}

// --- context menus ------------------------------------------------------------------------------

void MainWindow::activate_feature_row(QTreeWidgetItem* item) {
  const auto id = row_id(item);
  if (!id) return;
  if (item->data(0, kRoleKind).toInt() == 4) {
    // A graph node: open the graph with it selected.
    const Id volume = row_id(item, 1).value_or(Id{});
    if (edit_graph(volume)) select(Selection::of_node(*id, volume));
    return;
  }
  if (item->data(0, kRoleKind).toInt() != 3) return;
  const Id volume = row_id(item, 1).value_or(Id{});
  const auto* tree = tree_of(history_.project().document, volume);
  const auto* f = tree != nullptr ? tree->find(*id) : nullptr;
  if (f == nullptr) return;
  if (f->kind() == feature::FeatureKind::Sketch) {
    edit_sketch(volume, *id);
  } else {
    edit_feature(volume, *id);
  }
}

void MainWindow::feature_context_menu(const QPoint& pos) {
  QTreeWidgetItem* item = features_->itemAt(pos);
  const auto id = row_id(item);
  if (!id || item->data(0, kRoleKind).toInt() != 3) return;
  const Id volume = row_id(item, 1).value_or(Id{});
  const auto* tree = tree_of(history_.project().document, volume);
  const auto* f = tree != nullptr ? tree->find(*id) : nullptr;
  if (f == nullptr) return;
  const Id fid = *id;
  QMenu menu(this);
  if (f->kind() == feature::FeatureKind::Sketch) {
    menu.addAction(tr("Edit sketch"), this, [this, volume, fid] { edit_sketch(volume, fid); });
    menu.addAction(tr("Sketch plane…"), this, [this, volume, fid] { edit_feature(volume, fid); });
  } else {
    menu.addAction(tr("Edit…"), this, [this, volume, fid] { edit_feature(volume, fid); });
  }
  menu.addAction(tr("Rename…"), this, [this, volume, fid] {
    const auto* t = tree_of(history_.project().document, volume);
    if (t == nullptr || t->find(fid) == nullptr) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename feature"), tr("Name"), QLineEdit::Normal,
                                               qstr(t->label(fid)), &ok);
    if (ok) apply(rename_feature(volume, fid, sstr(name.trimmed())));
  });
  menu.addAction(f->suppressed ? tr("Unsuppress") : tr("Suppress"), this,
                 [this, volume, fid, s = f->suppressed] { apply(set_suppressed(volume, fid, !s)); });
  menu.addSeparator();
  menu.addAction(tr("Move up"), this, [this, volume, fid] { move_feature(volume, fid, -1); });
  menu.addAction(tr("Move down"), this, [this, volume, fid] { move_feature(volume, fid, +1); });
  menu.addSeparator();
  menu.addAction(tr("Delete"), this, [this, volume, fid] { apply(remove_feature(volume, fid)); });
  menu.exec(features_->viewport()->mapToGlobal(pos));
}

void MainWindow::move_feature(const Id& volume, const Id& feature, int delta) {
  const auto* tree = tree_of(history_.project().document, volume);
  if (tree == nullptr) return;
  const auto index = tree->index_of(feature);
  if (!index) return;
  const long long to = static_cast<long long>(*index) + delta;
  if (to < 0 || to >= static_cast<long long>(tree->features().size())) return;
  apply(app::move_feature(volume, feature, static_cast<std::size_t>(to)));
}

void MainWindow::volume_context_menu(const QPoint& pos) {
  QTreeWidgetItem* item = volumes_->itemAt(pos);
  const auto id = row_id(item);
  if (!id) return;
  QMenu menu(this);
  const Id vid = *id;
  if (item->data(0, kRoleKind).toInt() == 1) {
    menu.addAction(tr("Edit…"), this, [this, vid] { edit_connection(vid); });
    menu.addAction(tr("Delete"), this, [this, vid] { apply(remove_connection(vid)); });
    menu.exec(volumes_->viewport()->mapToGlobal(pos));
    return;
  }
  const model::Volume* v = history_.project().document.find_volume(vid);
  if (v == nullptr) return;
  menu.addAction(tr("Properties…"), this, [this, vid] { edit_volume(vid); });
  if (!v->keep_out) {
    if (v->tree() != nullptr || v->contents_empty()) {
      menu.addAction(v->tree() != nullptr ? tr("Edit contents in context") : tr("Fill by modelling"), this,
                     [this, vid] { edit_in_context(vid); });
    }
    const bool procedural = v->kind() == model::VolumeKind::Procedural;
    if (procedural || v->contents_empty()) {
      menu.addAction(procedural ? tr("Edit graph") : tr("Fill by procedure"), this, [this, vid] { edit_graph(vid); });
      menu.addAction(tr("Attach template…"), this, [this, vid] { attach_template(vid); });
    }
    if (procedural) menu.addAction(tr("Bake"), this, [this, vid] { bake(vid); });
    if (proc::is_baked(*v)) menu.addAction(tr("Unbake"), this, [this, vid] { unbake(vid); });
    menu.addAction(tr("Import contents…"), this, [this, vid] {
      const QString path = QFileDialog::getOpenFileName(this, tr("Import contents"), QString(),
                                                        tr("Geometry (*.step *.stp *.iges *.igs *.stl *.3mf *.obj)"));
      if (path.isEmpty()) return;
      std::ifstream f(fspath(path), std::ios::binary);
      const std::vector<char> chars((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      std::vector<std::byte> bytes(chars.size());
      std::transform(chars.begin(), chars.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
      const std::string format(eval::import_format(sstr(QFileInfo(path).suffix())));
      apply(import_contents(vid, std::move(bytes), format, session_->settings()));
    });
    if (!v->contents_empty()) menu.addAction(tr("Clear contents"), this, [this, vid] { apply(make_placeholder(vid)); });
    // M16: swap the contents' kind, nothing outside the volume touched.
    QMenu* swap = menu.addMenu(tr("Swap contents"));
    const auto now = contents_kind(*v);
    const std::pair<ContentsKind, const char*> kinds[] = {{ContentsKind::Placeholder, "Placeholder"},
                                                          {ContentsKind::Parametric, "Modelled"},
                                                          {ContentsKind::Procedural, "Procedural"},
                                                          {ContentsKind::Baked, "Baked"}};
    for (const auto& [k, label] : kinds) {
      const ContentsKind target = k;
      QAction* a = swap->addAction(tr(label), this, [this, vid, target] { swap_contents(vid, target); });
      a->setCheckable(true);
      a->setChecked(now == target);
      if (target == ContentsKind::Baked) a->setEnabled(now == ContentsKind::Procedural);
    }
    // M16: CAD habits, redirected.
    menu.addSeparator();
    menu.addAction(tr("Fillet…"), this, [this, vid] { guide(vid, Habit::Fillet); });
    menu.addAction(tr("Cut…"), this, [this, vid] { guide(vid, Habit::Cut); });
    menu.addAction(tr("Mate…"), this, [this, vid] { guide(vid, Habit::Mate); });
  }
  menu.addAction(v->keep_out ? tr("Unmark keep-out") : tr("Mark as keep-out"), this,
                 [this, vid, k = v->keep_out] { apply(set_keep_out(vid, !k)); });
  menu.addSeparator();
  menu.addAction(tr("Delete"), this, [this, vid] { apply(remove_volume(vid)); });
  menu.exec(volumes_->viewport()->mapToGlobal(pos));
}

// --- edit in context ------------------------------------------------------------------------------

bool MainWindow::edit_in_context(const Id& volume) {
  const model::Volume* v = history_.project().document.find_volume(volume);
  if (v == nullptr || v->keep_out) return false;
  if (v->tree() == nullptr) {
    if (!v->contents_empty()) {
      status_label_->setText(tr("%1 has %2 contents; clear them to fill it by modelling")
                                 .arg(qstr(v->name), qstr(model::to_string(v->kind())).toLower()));
      return false;
    }
    if (apply(make_parametric(volume)).has_errors()) return false;
    v = history_.project().document.find_volume(volume);
  }
  close_dialog();
  editing_volume_ = volume;
  std::vector<Id> inputs;
  for (const auto& s : history_.project().document.connections) {
    if (s.joins(volume)) inputs.push_back(s.id);
  }
  viewport_->set_context(volume, inputs);
  context_label_->setText(tr("Editing the contents of %1: only its boundary and connection surfaces can be "
                             "referenced")
                              .arg(qstr(v->name)));
  context_bar_->setVisible(true);
  leave_context_action_->setEnabled(true);
  select(Selection::of_volume(volume));
  refresh_volume_tree();
  return true;
}

void MainWindow::leave_context() {
  if (editing_volume_.is_null()) return;
  close_dialog();
  const Id was = editing_volume_;
  editing_volume_ = {};
  viewport_->set_context({}, {});
  context_bar_->setVisible(false);
  leave_context_action_->setEnabled(false);
  select(Selection::of_volume(was));
  refresh_volume_tree();
}

// --- dialogs ---------------------------------------------------------------------------------------

void MainWindow::start_dialog(EditDialog* d, std::function<Command()> make, std::function<bool()> commit_override) {
  close_dialog();
  dialog_ = d;
  dialog_command_ = std::move(make);
  d->setAttribute(Qt::WA_DeleteOnClose);
  connect(d, &EditDialog::changed, this, [this] { preview_timer_->start(); });
  connect(d, &EditDialog::commitRequested, this, [this, d, commit_override] {
    if (!d->complete()) {
      d->set_message(tr("Choose every reference and enter every number first."));
      return;
    }
    preview_timer_->stop();
    if (commit_override) {
      if (commit_override()) {
        dialog_ = nullptr;
        d->accept();
      }
      return;
    }
    const auto result = apply(dialog_command_());
    if (result.has_errors()) {
      d->set_message(qstr(first_error(result)));
      return;
    }
    dialog_ = nullptr;
    d->accept();
  });
  connect(d, &QDialog::rejected, this, [this, d] {
    preview_timer_->stop();
    if (dialog_ == d) dialog_ = nullptr;
    end_preview();
  });
  // M16: pick in view. The pick mode ends with the dialog.
  if (auto* fd = qobject_cast<FeatureDialog*>(d)) {
    connect(fd, &FeatureDialog::pickRequested, this, &MainWindow::on_pick_requested);
  }
  connect(d, &QDialog::finished, this, [this] {
    viewport_->set_pick_mode(PickMode::None);
    refresh_handles();
  });
  refresh_handles();
  d->show();
  if (!commit_override) preview_timer_->start();
}

void MainWindow::run_preview() {
  if (!dialog_ || !dialog_command_) return;
  if (!dialog_->complete()) return;
  core::Diagnostics why;
  auto preview = history_.try_apply(dialog_command_(), &why);
  if (!preview) {
    dialog_->set_message(qstr(first_error(why)));
    end_preview();
    return;
  }
  dialog_->set_message({});
  previewing_ = true;
  session_->update_project(std::move(preview));
}

void MainWindow::flush_preview() {
  if (preview_timer_->isActive()) {
    preview_timer_->stop();
    run_preview();
  }
}

void MainWindow::end_preview() {
  if (!previewing_) return;
  previewing_ = false;
  session_->update_project(history_.current());
}

void MainWindow::close_dialog() {
  if (dialog_) {
    EditDialog* d = dialog_;
    dialog_ = nullptr;
    d->reject();
  }
  end_preview();
}

FeatureDialog* MainWindow::insert_feature(feature::FeatureKind kind) {
  const SnapshotPtr s = session_->latest();
  if (!s || !session_->has_project()) return nullptr;
  const Id volume = editing_volume_;
  const auto* tree = tree_of(history_.project().document, volume);
  if (tree == nullptr) return nullptr;
  // M16: rounding generated contents is a CAD habit to redirect.
  if (volume.is_null() && selection_.kind == Selection::Kind::Volume &&
      (kind == feature::FeatureKind::Fillet || kind == feature::FeatureKind::Chamfer)) {
    if (const auto* v = history_.project().document.find_volume(selection_.id);
        v != nullptr && !v->keep_out && v->tree() == nullptr && !v->contents_empty()) {
      guide(selection_.id, Habit::Fillet);
      return nullptr;
    }
  }
  dialog_tree_ = volume;
  feature::Feature f;
  f.id = Id::random();
  switch (kind) {
    case feature::FeatureKind::Sketch: f.definition = feature::SketchFeature{}; break;
    case feature::FeatureKind::DatumPlane: f.definition = feature::DatumPlaneFeature{}; break;
    case feature::FeatureKind::DatumAxis: f.definition = feature::DatumAxisFeature{}; break;
    case feature::FeatureKind::DatumPoint: f.definition = feature::DatumPointFeature{}; break;
    case feature::FeatureKind::Extrude: f.definition = feature::ExtrudeFeature{}; break;
    case feature::FeatureKind::Revolve: f.definition = feature::RevolveFeature{}; break;
    case feature::FeatureKind::Sweep: f.definition = feature::SweepFeature{}; break;
    case feature::FeatureKind::Loft: f.definition = feature::LoftFeature{}; break;
    case feature::FeatureKind::Shell: f.definition = feature::ShellFeature{}; break;
    case feature::FeatureKind::Fillet: f.definition = feature::FilletFeature{}; break;
    case feature::FeatureKind::Chamfer: f.definition = feature::ChamferFeature{}; break;
    case feature::FeatureKind::LinearPattern: f.definition = feature::LinearPatternFeature{}; break;
    case feature::FeatureKind::CircularPattern: f.definition = feature::CircularPatternFeature{}; break;
  }
  ReferenceChoices choices = reference_choices(s->evaluation, volume);
  // Sensible first choices: the last sketch, a principal or first plane.
  if (auto* e = f.as<feature::ExtrudeFeature>(); e != nullptr && !choices.sketches.empty()) {
    e->sketch = choices.sketches.back().ref;
  }
  if (auto* r = f.as<feature::RevolveFeature>(); r != nullptr && !choices.sketches.empty()) {
    r->sketch = choices.sketches.back().ref;
  }
  if (auto* sk = f.as<feature::SketchFeature>(); sk != nullptr && !choices.planes.empty()) {
    sk->plane = choices.planes.front().ref;
  }
  auto* d = new FeatureDialog(f, std::move(choices), this, true);
  if (kind == feature::FeatureKind::Sketch) {
    // OK picks the plane, then sketch mode draws the sketch.
    start_dialog(d, {}, [this, d] {
      const feature::Feature chosen = d->feature();
      new_sketch(chosen.as<feature::SketchFeature>()->plane, chosen.name);
      return true;
    });
    return d;
  }
  start_dialog(d, [volume, d] { return add_feature(volume, d->feature()); });
  return d;
}

FeatureDialog* MainWindow::edit_feature(const Id& volume, const Id& feature) {
  const SnapshotPtr s = session_->latest();
  const auto* tree = tree_of(history_.project().document, volume);
  if (!s || tree == nullptr) return nullptr;
  const auto index = tree->index_of(feature);
  if (!index) return nullptr;
  const feature::Feature f = *tree->find(feature);
  dialog_tree_ = volume;
  auto* d = new FeatureDialog(f, reference_choices(s->evaluation, volume, *index), this);
  start_dialog(d, [volume, d] { return replace_feature(volume, d->feature()); });
  return d;
}

VolumeDialog* MainWindow::new_volume() {
  const SnapshotPtr s = session_->latest();
  if (!s) return nullptr;
  std::vector<std::string> taken;
  for (const auto& v : history_.project().document.volumes) taken.push_back(v.name);
  model::Volume v = volume_from_features(unique_name("Volume", taken), {});
  // A layout solid that is selected is the likely boundary.
  if (selection_.kind == Selection::Kind::Feature && selection_.volume.is_null()) {
    std::get<model::FeatureBoundary>(v.boundary).features.push_back(selection_.id);
  }
  auto* d = new VolumeDialog(v, s->evaluation, this);
  start_dialog(d, [d] { return add_volume(d->value()); });
  return d;
}

VolumeDialog* MainWindow::edit_volume(const Id& volume) {
  const SnapshotPtr s = session_->latest();
  const model::Volume* v = history_.project().document.find_volume(volume);
  if (!s || v == nullptr) return nullptr;
  auto* d = new VolumeDialog(*v, s->evaluation, this);
  start_dialog(d, [d] { return update_volume(d->value()); });
  return d;
}

ConnectionDialog* MainWindow::new_connection() {
  const SnapshotPtr s = session_->latest();
  if (!s) return nullptr;
  std::vector<std::string> taken;
  for (const auto& c : history_.project().document.connections) taken.push_back(c.name);
  model::ConnectionSurface c;
  c.id = Id::random();
  c.name = unique_name("Surface", taken);
  c.geometry = model::SketchPatch{};
  auto* d = new ConnectionDialog(c, s->evaluation, this);
  start_dialog(d, [d] { return add_connection(d->value()); });
  return d;
}

ConnectionDialog* MainWindow::edit_connection(const Id& surface) {
  const SnapshotPtr s = session_->latest();
  const model::ConnectionSurface* c = history_.project().document.find_connection(surface);
  if (!s || c == nullptr) return nullptr;
  auto* d = new ConnectionDialog(*c, s->evaluation, this);
  start_dialog(d, [d] { return update_connection(d->value()); });
  return d;
}

KeepOutDialog* MainWindow::new_keep_out() {
  const SnapshotPtr s = session_->latest();
  if (!s) return nullptr;
  std::vector<std::string> taken;
  for (const auto& k : history_.project().document.keep_outs) taken.push_back(k.name);
  model::KeepOut k;
  k.id = Id::random();
  k.name = unique_name("Keep-out", taken);
  auto* d = new KeepOutDialog(k, s->evaluation, this);
  start_dialog(d, [d] { return add_keep_out(d->value()); });
  return d;
}

LinkDialog* MainWindow::new_link() {
  const SnapshotPtr s = session_->latest();
  if (!s) return nullptr;
  model::Link l;
  l.id = Id::random();
  auto* d = new LinkDialog(l, s->evaluation, this);
  start_dialog(d, [d] { return add_link(d->value()); });
  return d;
}

// --- procedural graphs (M15) ---------------------------------------------------------------------

bool MainWindow::edit_graph(const Id& volume) {
  const model::Volume* v = history_.project().document.find_volume(volume);
  if (v == nullptr || v->keep_out) return false;
  if (proc::is_baked(*v)) {
    status_label_->setText(tr("%1 is baked; unbake it to edit its graph").arg(qstr(v->name)));
    return false;
  }
  if (v->kind() != model::VolumeKind::Procedural) {
    if (!v->contents_empty()) {
      status_label_->setText(tr("%1 has %2 contents; clear them to fill it by procedure")
                                 .arg(qstr(v->name), qstr(model::to_string(v->kind())).toLower()));
      return false;
    }
    if (apply(make_procedural(volume, starter_graph())).has_errors()) return false;
  }
  if (!graph_of(history_.project().document, volume)) {
    status_label_->setText(tr("%1's graph can't be read").arg(qstr(v->name)));
    return false;
  }
  if (!editing_volume_.is_null()) leave_context();
  if (graph_volume_ != volume) clear_node_preview();
  graph_volume_ = volume;
  const SnapshotPtr s = session_->latest();
  node_editor_->show_graph(history_.project(), s ? &s->evaluation : nullptr, volume);
  if (!graph_dock_->isVisible()) graph_dock_->show();
  graph_dock_->raise();
  resizeDocks({graph_dock_}, {std::max(320, height() / 3)}, Qt::Vertical);
  if (!(selection_.kind == Selection::Kind::Node && selection_.volume == volume)) select(Selection::of_volume(volume));
  status_label_->setText(tr("Editing the graph of %1").arg(qstr(history_.project().document.find_volume(volume)->name)));
  return true;
}

void MainWindow::leave_graph() {
  if (graph_volume_.is_null()) return;
  const Id was = graph_volume_;
  graph_volume_ = {};
  clear_node_preview();
  node_editor_->clear();
  if (selection_.kind == Selection::Kind::Node && selection_.volume == was) select(Selection::of_volume(was));
  refresh_inspector();
}

TemplateDialog* MainWindow::attach_template(const Id& volume) {
  const model::Volume* v = history_.project().document.find_volume(volume);
  if (v == nullptr || v->keep_out) return nullptr;
  if (!v->contents_empty() && v->kind() != model::VolumeKind::Procedural) {
    status_label_->setText(tr("%1 has %2 contents; clear them to attach a template")
                               .arg(qstr(v->name), qstr(model::to_string(v->kind())).toLower()));
    return nullptr;
  }
  auto* d = new TemplateDialog(history_.project().document, volume, this);
  start_dialog(d, [d, volume] {
    auto g = d->graph();
    return make_procedural(volume, g ? *g : proc::Graph{}, "Attach template " + d->template_name());
  });
  connect(d, &QDialog::accepted, this, [this, volume] {
    // Show what was attached.
    if (graph_volume_ == volume || graph_volume_.is_null()) edit_graph(volume);
  });
  return d;
}

core::Diagnostics MainWindow::bake(const Id& volume) {
  core::Diagnostics d;
  const SnapshotPtr s = session_->latest();
  const model::Volume* v = history_.project().document.find_volume(volume);
  const eval::VolumeResult* r = s ? s->evaluation.find(volume) : nullptr;
  const mesh::Mesh* m = r != nullptr ? proc::contents_mesh(*r) : nullptr;
  if (v == nullptr || v->kind() != model::VolumeKind::Procedural) {
    d.add(core::Diagnostic::error("proc.bake.kind", "Only a procedural volume can be baked."));
  } else if (!s || s->preview || previewing_ || s->project != history_.current() || r == nullptr ||
             r->status != model::VolumeStatus::Current || r->stale_contents || m == nullptr) {
    d.add(core::Diagnostic::error("app.bake.not_generated",
                                  fmt::format("Wait for '{}' to finish generating before baking it.", v->name)));
  }
  if (d.has_errors()) {
    show_diagnostics(d, tr("Bake"));
    status_label_->setText(qstr(d.items().front().message));
    return d;
  }
  const bool was_open = graph_volume_ == volume;
  d = apply(bake_volume(volume, *m));
  if (!d.has_errors() && was_open) leave_graph();
  return d;
}

core::Diagnostics MainWindow::unbake(const Id& volume) {
  const core::Diagnostics d = apply(unbake_volume(volume));
  return d;
}

void MainWindow::preview_node(const Id& node, const std::string& port) {
  const SnapshotPtr s = session_->latest();
  if (!s || graph_volume_.is_null() || node_editor_->graph().find_node(node) == nullptr) return;
  NodePreviewRequest request;
  request.snapshot = s;
  request.volume = graph_volume_;
  request.graph = node_editor_->graph();
  request.node = node;
  request.port = port;
  request.options = node_editor_->preview_options();
  preview_port_ = port;
  session_->preview_node(std::move(request));
}

void MainWindow::clear_node_preview() {
  session_->cancel_preview();
  last_preview_.reset();
  preview_port_.clear();
  viewport_->clear_overlay();
  node_editor_->clear_preview();
}

// --- sketch mode --------------------------------------------------------------------------------

SketchMode* MainWindow::new_sketch(const feature::PlaneRef& plane, const std::string& name) {
  const auto* tree = tree_of(history_.project().document, editing_volume_);
  if (tree == nullptr) return nullptr;
  std::size_t n = 1;
  for (const auto& f : tree->features()) n += f.kind() == feature::FeatureKind::Sketch ? 1 : 0;
  sketch::Sketch sk(Id::random(), name.empty() ? fmt::format("Sketch {}", n) : name);
  feature::Feature f;
  f.id = sk.id();
  f.definition = feature::SketchFeature{std::move(sk), plane};
  return open_sketch_mode(editing_volume_, std::move(f), true);
}

SketchMode* MainWindow::edit_sketch(const Id& volume, const Id& feature) {
  const auto* tree = tree_of(history_.project().document, volume);
  const auto* f = tree != nullptr ? tree->find(feature) : nullptr;
  if (f == nullptr || f->as<feature::SketchFeature>() == nullptr) return nullptr;
  return open_sketch_mode(volume, *f, false);
}

SketchMode* MainWindow::open_sketch_mode(const Id& volume, feature::Feature f, bool is_new) {
  if (sketch_mode_ != nullptr) finish_sketch(false);
  close_dialog();
  const SnapshotPtr s = session_->latest();
  std::map<std::string, double, std::less<>> values;
  if (s) values = variable_values(s->evaluation, volume);
  auto lookup = [values](std::string_view name) -> std::optional<double> {
    const auto it = values.find(name);
    if (it == values.end()) return std::nullopt;
    return it->second;
  };
  const auto& sk = f.as<feature::SketchFeature>()->sketch;
  QString where;
  if (s) where = qstr(reference_choices(s->evaluation, volume).label_of(f.as<feature::SketchFeature>()->plane));
  const QString title = qstr(sk.name().empty() ? std::string("Sketch") : sk.name()) +
                        (where.isEmpty() ? QString() : tr(" on %1").arg(where));
  auto editor = std::make_unique<SketchEditor>(sk, session_->settings(), lookup);
  sketch_mode_ = new SketchMode(std::move(editor), title);
  central_->addWidget(sketch_mode_);
  central_->setCurrentWidget(sketch_mode_);
  sketch_session_ = SketchSession{volume, std::move(f), is_new};
  connect(sketch_mode_, &SketchMode::finished, this, &MainWindow::finish_sketch);
  sketch_mode_->view().setFocus();
  status_label_->setText(tr("Sketch mode: draw, constrain, then Finish sketch"));
  return sketch_mode_;
}

void MainWindow::finish_sketch(bool accept) {
  if (sketch_mode_ == nullptr || !sketch_session_) return;
  if (accept) {
    const sketch::Sketch& sk = sketch_mode_->editor().sketch();
    core::Diagnostics d;
    if (sketch_session_->is_new) {
      feature::Feature f = sketch_session_->feature;
      f.as<feature::SketchFeature>()->sketch = sk;
      d = apply(add_feature(sketch_session_->volume, std::move(f)));
    } else if (sketch_mode_->editor().changed()) {
      d = apply(set_sketch(sketch_session_->volume, sketch_session_->feature.id, sk));
    }
    if (d.has_errors()) return;  // stay in sketch mode; the reason is shown
  }
  central_->setCurrentWidget(viewport_);
  central_->removeWidget(sketch_mode_);
  sketch_mode_->deleteLater();
  sketch_mode_ = nullptr;
  sketch_session_.reset();
  refresh_edit_actions();
}

// --- interactive regeneration and guidance (M16) -----------------------------------------------------

QString MainWindow::status_text() const { return status_label_->text(); }

void MainWindow::refresh_handles() {
  if (drag_) return;  // the handles stay as they were while one is dragged
  std::vector<DimensionHandle> found;
  const SnapshotPtr s = session_->latest();
  if (s && !s->drag && !dialog_ && sketch_mode_ == nullptr && selection_.kind == Selection::Kind::Feature &&
      viewport_->pick_mode() == PickMode::None) {
    found = app::dimension_handles(s->evaluation, selection_.volume, selection_.id);
  }
  handles_ = std::move(found);
  std::vector<ViewHandle> view;
  view.reserve(handles_.size());
  for (const auto& h : handles_) view.push_back({h.anchor, h.direction});
  viewport_->set_handles(std::move(view));
}

bool MainWindow::begin_drag(const DimensionHandle& handle) {
  if (!session_->has_project() || sketch_mode_ != nullptr) return false;
  drag_ = Drag{handle, handle.value, false};
  close_dialog();
  dismiss_guidance();
  status_label_->setText(tr("Dragging %1").arg(qstr(handle.label)));
  return true;
}

void MainWindow::drag_to(double value) {
  if (!drag_) return;
  drag_->value = value;
  drag_->moved = true;
  core::Diagnostics why;
  auto preview = history_.try_apply(drag_command(drag_->handle, value, session_->settings()), &why);
  if (!preview) {
    status_label_->setText(tr("Can't drag to %1: %2").arg(value, 0, 'g', 6).arg(qstr(first_error(why))));
    return;
  }
  session_->drag_preview(std::move(preview));
}

core::Diagnostics MainWindow::end_drag(bool commit) {
  if (!drag_) return {};
  const Drag d = *drag_;
  drag_.reset();
  core::Diagnostics result;
  if (commit && d.moved && d.value != d.handle.value) {
    // One undoable step; showing it regenerates in full.
    result = apply(drag_command(d.handle, d.value, session_->settings()));
    if (result.has_errors()) session_->update_project(history_.current());
  } else {
    session_->update_project(history_.current());
  }
  return result;
}

void MainWindow::on_handle_dragged(int index, double distance, bool finished) {
  if (!drag_) {
    if (index < 0 || static_cast<std::size_t>(index) >= handles_.size()) return;
    if (!begin_drag(handles_[static_cast<std::size_t>(index)])) return;
  }
  const double value = dragged_value(drag_->handle, distance);
  if (value != drag_->value) drag_to(value);
  if (finished) end_drag(true);
}

void MainWindow::on_pick_requested(int field) {
  auto* fd = qobject_cast<FeatureDialog*>(dialog_.data());
  const SnapshotPtr s = session_->latest();
  if (fd == nullptr || field < 0 || !s) {
    viewport_->set_pick_mode(PickMode::None);
    refresh_handles();
    return;
  }
  handles_.clear();
  viewport_->set_handles({});
  const bool edges = fd->pick_kind(field) == FeatureDialog::PickKind::Edge;
  viewport_->set_pick_mode(edges ? PickMode::Edge : PickMode::Face, pickable_items(s->evaluation, dialog_tree_));
  status_label_->setText(edges ? tr("Pick edges in the viewport for %1").arg(fd->pick_label(field))
                               : tr("Pick a face in the viewport for %1").arg(fd->pick_label(field)));
}

void MainWindow::on_part_picked(const PickedPart& part) {
  auto* fd = qobject_cast<FeatureDialog*>(dialog_.data());
  const SnapshotPtr s = session_->latest();
  if (fd == nullptr || fd->picking() < 0 || !s) return;
  last_pick_ = resolve_pick(s->scene, s->evaluation, dialog_tree_, fd->choices(), part);
  if (fd->apply_pick(*last_pick_)) {
    status_label_->setText(tr("Picked %1").arg(qstr(last_pick_->label)));
  } else {
    status_label_->setText(fd->message());
  }
}

core::Diagnostics MainWindow::swap_contents(const Id& volume, ContentsKind kind) {
  // Baking takes the procedure's current mesh, which only the screen has.
  if (kind == ContentsKind::Baked) return bake(volume);
  return apply(app::swap_contents(volume, kind));
}

std::optional<Guidance> MainWindow::guide(const Id& volume, Habit habit) {
  guidance_ = app::guide(history_.project().document, volume, habit, editing_volume_);
  if (!guidance_) {
    guidance_bar_->setVisible(false);
    return std::nullopt;
  }
  guidance_label_->setText(QStringLiteral("<b>%1</b><br>%2")
                               .arg(qstr(guidance_->title).toHtmlEscaped(), qstr(guidance_->text).toHtmlEscaped()));
  guidance_action_->setText(qstr(guidance_->action_label));
  guidance_bar_->setVisible(true);
  status_label_->setText(qstr(guidance_->title));
  return guidance_;
}

void MainWindow::dismiss_guidance() {
  guidance_.reset();
  if (guidance_bar_ != nullptr) guidance_bar_->setVisible(false);
}

void MainWindow::act_on_guidance() {
  if (!guidance_) return;
  const Guidance g = *guidance_;
  dismiss_guidance();
  switch (g.action) {
    case Guidance::Action::EditGraph: edit_graph(g.volume); break;
    case Guidance::Action::Unbake: unbake(g.volume); break;
    case Guidance::Action::EditInContext: {
      const model::Volume* v = history_.project().document.find_volume(g.volume);
      if (v == nullptr) return;
      // Imported contents give way to modelled ones (undoable).
      if (v->tree() == nullptr && !v->contents_empty() &&
          apply(app::swap_contents(g.volume, ContentsKind::Parametric)).has_errors()) {
        return;
      }
      if (edit_in_context(g.volume) && g.habit == Habit::Fillet) insert_feature(feature::FeatureKind::Fillet);
      break;
    }
    case Guidance::Action::AddKeepOut: new_keep_out(); break;
    case Guidance::Action::UseConnection: select(Selection::of_surface(g.target)); break;
    case Guidance::Action::AddConnection: new_connection(); break;
  }
}

// --- files -------------------------------------------------------------------------------------------

void MainWindow::add_recent(const std::filesystem::path& path) {
  QSettings settings(QStringLiteral("Volumetric Studio"), QStringLiteral("vs_app"));
  QStringList list = settings.value(QStringLiteral("recent")).toStringList();
  const QString p = QFileInfo(qpath(path)).absoluteFilePath();
  list.removeAll(p);
  list.prepend(p);
  while (list.size() > kRecentMax) list.removeLast();
  settings.setValue(QStringLiteral("recent"), list);
  rebuild_recent_menu();
}

void MainWindow::rebuild_recent_menu() {
  recent_menu_->clear();
  QSettings settings(QStringLiteral("Volumetric Studio"), QStringLiteral("vs_app"));
  const QStringList list = settings.value(QStringLiteral("recent")).toStringList();
  for (const QString& p : list) {
    recent_menu_->addAction(p, this, [this, p] { open_project(fspath(p)); });
  }
  recent_menu_->setEnabled(!list.isEmpty());
}

void MainWindow::open_dialog_box() {
  // A project is a folder or a zip file, so offer both.
  QFileDialog dialog(this, tr("Open project"));
  dialog.setNameFilters({tr("Projects (*.vsproj project.json)"), tr("All files (*)")});
  dialog.setFileMode(QFileDialog::ExistingFile);
  if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
  QFileInfo info(dialog.selectedFiles().front());
  // Picking a folder project's project.json opens the folder.
  const QString path = info.fileName() == QLatin1String("project.json") ? info.absolutePath() : info.absoluteFilePath();
  open_project(fspath(path));
}

bool MainWindow::save_caches() const { return save_caches_action_ != nullptr && save_caches_action_->isChecked(); }

void MainWindow::set_save_caches(bool on) {
  if (save_caches_action_ != nullptr) save_caches_action_->setChecked(on);
}

core::Diagnostics MainWindow::save_to(const std::filesystem::path& path) {
  // Caches come from the evaluation on screen: keys name exactly the inputs
  // each entry was made from, so entries never go wrong, at worst unused.
  // With the option off, saving removes any the project had.
  model::CacheFiles cache;
  if (save_caches()) {
    if (const SnapshotPtr s = session_->latest()) cache = eval::project_cache_files(s->evaluation);
  }
  const auto d = session_->save(path, &history_.project(), &cache);
  if (!d.has_errors()) history_.mark_saved();
  update_title();
  return d;
}

void MainWindow::save() {
  if (!session_->has_project()) return;
  if (session_->path().empty()) return save_as();
  const auto d = save_to(session_->path());
  report(d, tr("Saved %1").arg(qpath(session_->path())));
}

void MainWindow::save_as() {
  if (!session_->has_project()) return;
  const QString path = QFileDialog::getSaveFileName(this, tr("Save project as"), QString(), tr("Project (*.vsproj)"));
  if (path.isEmpty()) return;
  const auto d = save_to(fspath(path));
  if (!d.has_errors()) add_recent(fspath(path));
  report(d, tr("Saved %1").arg(path));
}

core::Diagnostics MainWindow::import_file(const std::filesystem::path& path) {
  core::Diagnostics d;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    d.add(core::Diagnostic::error("app.import.io", "Can't read " + sstr(qpath(path))));
    show_diagnostics(d, tr("Import"));
    return d;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(text.size());
  std::transform(text.begin(), text.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
  const QFileInfo info(qpath(path));
  const std::string format(eval::import_format(sstr(info.suffix())));
  std::vector<std::string> taken;
  for (const auto& v : history_.project().document.volumes) taken.push_back(v.name);
  const std::string name = unique_name(sstr(info.completeBaseName()), taken);
  return apply(app::import_volume(std::move(bytes), format, name, session_->settings()));
}

void MainWindow::import_dialog() {
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Import as new volume"), QString(),
      tr("Geometry (*.step *.stp *.iges *.igs *.stl *.3mf *.obj);;All files (*)"));
  if (path.isEmpty()) return;
  const auto d = import_file(fspath(path));
  if (!d.has_errors()) status_label_->setText(tr("Imported %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::check_then(const QString& what, std::function<void()> go) {
  validate([this, what, go = std::move(go)](const ShipCheck& check) {
    if (!check.ok()) {
      QMessageBox box(QMessageBox::Warning, what, qstr(summary(check)), QMessageBox::Ok, this);
      box.setInformativeText(tr("The Diagnostics panel lists every problem; click one to go to it."));
      box.exec();
      return;
    }
    if (check.warnings > 0 &&
        QMessageBox::question(this, what, qstr(summary(check)) + QStringLiteral("\n\n") + tr("Export anyway?")) !=
            QMessageBox::Yes) {
      return;
    }
    go();
  });
}

void MainWindow::export_dialog(ExportFormat format) {
  const bool stl = format == ExportFormat::Stl;
  check_then(stl ? tr("Export STL") : tr("Export 3MF"), [this, stl, format] {
    const QString path = QFileDialog::getSaveFileName(this, stl ? tr("Export STL") : tr("Export 3MF"), QString(),
                                                      stl ? tr("STL (*.stl)") : tr("3MF (*.3mf)"));
    if (path.isEmpty()) return;
    export_to(fspath(path), format);
  });
}

void MainWindow::export_step_dialog() {
  check_then(tr("Export STEP"), [this] {
    // The user's choice for procedural volumes, and what else to include.
    QDialog options(this);
    options.setWindowTitle(tr("Export STEP"));
    auto* layout = new QVBoxLayout(&options);
    layout->addWidget(new QLabel(tr("Procedural volumes go in as:"), &options));
    auto* as_mesh = new QRadioButton(tr("Mesh bodies (the generated triangles)"), &options);
    auto* as_boundary = new QRadioButton(tr("Their boundary only (B-rep)"), &options);
    as_mesh->setChecked(true);
    layout->addWidget(as_mesh);
    layout->addWidget(as_boundary);
    auto* boundaries = new QCheckBox(tr("Also include every volume's boundary"), &options);
    auto* surfaces = new QCheckBox(tr("Also include connection surfaces"), &options);
    layout->addWidget(boundaries);
    layout->addWidget(surfaces);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &options);
    connect(buttons, &QDialogButtonBox::accepted, &options, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &options, &QDialog::reject);
    layout->addWidget(buttons);
    if (options.exec() != QDialog::Accepted) return;
    eval::StepExportOptions o;
    o.procedural = as_boundary->isChecked() ? eval::ProceduralAs::Boundary : eval::ProceduralAs::Mesh;
    o.boundaries = boundaries->isChecked();
    o.connection_surfaces = surfaces->isChecked();
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Export STEP"), QString(), tr("STEP (*.step *.stp)"));
    if (path.isEmpty()) return;
    export_step_to(fspath(path), o);
  });
}

void MainWindow::export_step_to(const std::filesystem::path& path, const eval::StepExportOptions& options,
                                std::function<void(const eval::StepExport&)> done) {
  const SnapshotPtr s = session_->latest();
  if (!s || s->preview || session_->busy() || previewing_) {
    eval::StepExport r;
    r.diagnostics.add(core::Diagnostic::error("app.export.busy",
                                              "Wait for the evaluation to finish (and close the open dialog) "
                                              "before exporting."));
    report(r.diagnostics, {});
    if (done) done(r);
    return;
  }
  status_label_->setText(tr("Exporting…"));
  auto result = std::make_shared<eval::StepExport>();
  session_->run_in_background([s, result, path, options] { *result = eval::export_step(s->evaluation, path, options); },
                              [this, result, path, done] {
                                report(result->diagnostics,
                                       tr("Exported %1 (%2 parts: %3 B-rep, %4 mesh, %5 boundary)")
                                           .arg(qpath(path))
                                           .arg(result->parts.size())
                                           .arg(result->breps)
                                           .arg(result->meshes)
                                           .arg(result->boundaries));
                                if (done) done(*result);
                              });
}

void MainWindow::export_to(const std::filesystem::path& path, ExportFormat format,
                           std::function<void(const ExportResult&)> done) {
  const SnapshotPtr s = session_->latest();
  if (!s || s->preview || session_->busy() || previewing_) {
    ExportResult r;
    r.diagnostics.add(core::Diagnostic::error("app.export.busy",
                                              "Wait for the evaluation to finish (and close the open dialog) "
                                              "before exporting."));
    report(r.diagnostics, {});
    if (done) done(r);
    return;
  }
  status_label_->setText(tr("Exporting…"));
  auto result = std::make_shared<ExportResult>();
  const core::ProductSettings settings = session_->settings();
  session_->run_in_background(
      [s, result, path, format, settings] {
        *result = export_mesh(s->evaluation, s->project->blobs, settings, format);
        if (result->ok()) result->diagnostics.append(write_file(path, result->bytes));
      },
      [this, result, path, done] {
        report(result->diagnostics, tr("Exported %1 (%2 triangles, %3 objects)")
                                        .arg(qpath(path))
                                        .arg(result->triangles)
                                        .arg(result->objects));
        if (done) done(*result);
      });
}

void MainWindow::validate(std::function<void(const ShipCheck&)> done) {
  const SnapshotPtr s = session_->latest();
  if (!s) return;
  status_label_->setText(tr("Validating…"));
  auto check = std::make_shared<ShipCheck>();
  const core::ProductSettings settings = session_->settings();
  const bool interactive = !done;
  session_->run_in_background(
      [s, check, settings] { *check = check_for_export(*s->project, s->evaluation, settings); },
      [this, check, done, interactive] {
        last_check_ = *check;
        show_diagnostics(check->diagnostics, tr("Validation"));
        status_label_->setText(qstr(summary(*check)));
        if (done) {
          done(*check);
        } else if (interactive) {
          QMessageBox::information(this, tr("Validate"), qstr(summary(*check)));
        }
      });
}

void MainWindow::report(const core::Diagnostics& diagnostics, const QString& what) {
  if (diagnostics.has_errors()) {
    show_diagnostics(diagnostics, tr("Export"));
    status_label_->setText(tr("Failed: %1").arg(qstr(diagnostics.items().front().message)));
  } else if (!what.isEmpty()) {
    status_label_->setText(what);
  }
}

}  // namespace vs::app

#include "main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QTreeWidget>

#include <fmt/format.h>

#include "viewport.hpp"
#include "vs/core/version.hpp"
#include "vs/validate/validate.hpp"

namespace vs::app {

namespace {

constexpr int kRecentMax = 8;
constexpr int kRoleKind = Qt::UserRole;      // 0 volume, 1 surface, 2 feature/node row, 3 diagnostic
constexpr int kRoleId = Qt::UserRole + 1;    // the subject's id
constexpr int kRoleIndex = Qt::UserRole + 2; // feature row or diagnostic index

QString qstr(std::string_view s) {
  return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

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

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  session_ = new Session({}, this);
  session_->set_preparer(&Viewport::prepare);
  viewport_ = new Viewport(this);
  setCentralWidget(viewport_);
  build_docks();
  build_menus();

  status_label_ = new QLabel(tr("No project"));
  frame_label_ = new QLabel;
  statusBar()->addWidget(status_label_, 1);
  statusBar()->addPermanentWidget(frame_label_);

  state_timer_ = new QTimer(this);
  state_timer_->setInterval(100);
  connect(state_timer_, &QTimer::timeout, this, &MainWindow::refresh_states);

  connect(session_, &Session::evaluated, this, &MainWindow::show_snapshot);
  connect(session_, &Session::evaluationStarted, this, [this] {
    status_label_->setText(tr("Regenerating…"));
    cancel_action_->setEnabled(true);
    state_timer_->start();
  });
  connect(session_, &Session::progressed, this, [this](int done, int total) {
    status_label_->setText(tr("Regenerating… %1 of %2 volumes").arg(done).arg(total));
  });
  connect(session_, &Session::projectChanged, this, [this] {
    fitted_ = false;
    selection_ = {};
    update_title();
  });
  connect(viewport_, &Viewport::picked, this, [this](core::Id subject) {
    const SnapshotPtr s = session_->latest();
    if (subject.is_null() || !s) {
      select(Selection::none());
    } else if (s->evaluation.document.find_connection(subject) != nullptr) {
      select(Selection::of_surface(subject));
    } else {
      select(Selection::of_volume(subject));
    }
  });
  connect(viewport_, &Viewport::frameRendered, this, [this](double ms) {
    frame_label_->setText(tr("frame %1 ms").arg(ms, 0, 'f', 1));
  });

  resize(1440, 900);
  update_title();
}

MainWindow::~MainWindow() = default;

void MainWindow::build_menus() {
  QMenu* file = menuBar()->addMenu(tr("&File"));
  file->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::open_dialog);
  recent_menu_ = file->addMenu(tr("Open &Recent"));
  rebuild_recent_menu();
  file->addSeparator();
  file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::save);
  file->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, &MainWindow::save_as);
  file->addSeparator();
  file->addAction(tr("Export &STL…"), this, [this] { export_dialog(ExportFormat::Stl); });
  file->addAction(tr("Export &3MF…"), this, [this] { export_dialog(ExportFormat::ThreeMf); });
  file->addSeparator();
  file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

  QMenu* model = menuBar()->addMenu(tr("&Model"));
  model->addAction(tr("&Regenerate"), QKeySequence(Qt::Key_F5), session_, &Session::regenerate);
  cancel_action_ = model->addAction(tr("&Cancel"), QKeySequence(Qt::Key_Escape), session_, &Session::cancel);
  cancel_action_->setEnabled(false);
  model->addAction(tr("&Validate"), QKeySequence(Qt::CTRL | Qt::Key_R), this, &MainWindow::validate);

  QMenu* view = menuBar()->addMenu(tr("&View"));
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
  dock(tr("Volumes"), "volumes", volumes_, Qt::LeftDockWidgetArea);
  connect(volumes_, &QTreeWidget::itemSelectionChanged, this, [this] {
    if (syncing_) return;
    const auto items = volumes_->selectedItems();
    if (items.isEmpty()) return select(Selection::none());
    const auto id = core::Id::parse(items.front()->data(0, kRoleId).toString().toStdString());
    if (!id) return select(Selection::none());
    select(items.front()->data(0, kRoleKind).toInt() == 1 ? Selection::of_surface(*id)
                                                          : Selection::of_volume(*id));
  });

  features_ = make_tree({tr("Feature"), tr("Kind"), tr("Status")});
  features_->setRootIsDecorated(false);
  features_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  features_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  dock(tr("Features"), "features", features_, Qt::LeftDockWidgetArea);
  connect(features_, &QTreeWidget::itemSelectionChanged, this, [this] {
    if (syncing_) return;
    const auto items = features_->selectedItems();
    if (items.isEmpty()) return;
    const auto id = core::Id::parse(items.front()->data(0, kRoleId).toString().toStdString());
    const auto volume = core::Id::parse(items.front()->data(1, kRoleId).toString().toStdString());
    if (!id) return;
    const bool node = items.front()->data(0, kRoleKind).toInt() == 4;
    const core::Id v = volume ? *volume : core::Id{};
    select(node ? Selection::of_node(*id, v) : Selection::of_feature(*id, v));
  });

  inspector_ = make_tree({tr("Property"), tr("Value")});
  inspector_->setAlternatingRowColors(false);
  inspector_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  inspector_->setIndentation(12);
  inspector_->setMinimumWidth(300);
  dock(tr("Inspector"), "inspector", inspector_, Qt::RightDockWidgetArea);

  diagnostics_ = make_tree({tr("Severity"), tr("Code"), tr("Message"), tr("Act on")});
  diagnostics_->setRootIsDecorated(false);
  diagnostics_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  diagnostics_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  diagnostics_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  diagnostics_->header()->setStretchLastSection(false);
  dock(tr("Diagnostics"), "diagnostics", diagnostics_, Qt::BottomDockWidgetArea);
  connect(diagnostics_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
    activate_diagnostic(diagnostics_->indexOfTopLevelItem(item));
  });
}

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

void MainWindow::select(const Selection& selection) {
  selection_ = selection;
  viewport_->set_highlight(viewport_subject(selection));
  sync_tree_selection();
  refresh_features();
  refresh_inspector();
}

void MainWindow::show_snapshot(const SnapshotPtr& snapshot) {
  const bool fit = !fitted_ && !snapshot->scene.items.empty();
  viewport_->show_scene(snapshot->scene, snapshot->prepared, fit);
  fitted_ = fitted_ || fit;
  // Keep the selection if it still exists.
  const auto& doc = snapshot->evaluation.document;
  if ((selection_.kind == Selection::Kind::Volume && doc.find_volume(selection_.id) == nullptr) ||
      (selection_.kind == Selection::Kind::ConnectionSurface && doc.find_connection(selection_.id) == nullptr)) {
    selection_ = {};
  }
  refresh_volume_tree();
  refresh_features();
  refresh_inspector();
  viewport_->set_highlight(viewport_subject(selection_));
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
  const auto& st = snapshot->evaluation.stats;
  status_label_->setText(
      tr("%1 — %2 volumes, %3 regenerated, %4 from cache, %5 ms (display %6 ms)%7")
          .arg(qstr(doc.name))
          .arg(doc.volumes.size())
          .arg(st.regenerated)
          .arg(st.cache_hits)
          .arg(snapshot->evaluate_ms, 0, 'f', 0)
          .arg(snapshot->scene_ms, 0, 'f', 0)
          .arg(snapshot->evaluation.cancelled ? tr(" — cancelled") : QString()));
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
    core::Id volume;
    if (selection_.kind == Selection::Kind::Volume) volume = selection_.id;
    if (selection_.kind == Selection::Kind::Feature || selection_.kind == Selection::Kind::Node) {
      volume = selection_.volume;
    }
    const auto* v = s->evaluation.document.find_volume(volume);
    features_->setHeaderLabels({v != nullptr ? tr("%1 — features").arg(qstr(v->name)) : tr("Layout features"),
                                tr("Kind"), tr("Status")});
    for (const FeatureRow& row : feature_rows(s->evaluation, volume)) {
      auto* item = new QTreeWidgetItem(features_);
      item->setText(0, qstr(row.name));
      item->setText(1, qstr(row.kind));
      item->setText(2, qstr(row.status));
      if (row.status == "failed") item->setForeground(2, QColor(208, 48, 48));
      item->setData(0, kRoleKind, row.selection.kind == Selection::Kind::Node ? 4 : 3);
      item->setData(0, kRoleId, qstr(row.selection.id.to_string()));
      item->setData(1, kRoleId, qstr(row.selection.volume.to_string()));
      if (row.selection == selection_) item->setSelected(true);
    }
  }
  syncing_ = false;
}

void MainWindow::refresh_inspector() {
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
  const SnapshotPtr s = session_->latest();
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
  (void)s;
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
    title = qstr(session_->project().document.name) + QStringLiteral(" — ") + title;
  }
  setWindowTitle(title);
}

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

void MainWindow::open_dialog() {
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

void MainWindow::save() {
  if (!session_->has_project()) return;
  if (session_->path().empty()) return save_as();
  report(session_->save(session_->path()), tr("Saved %1").arg(qpath(session_->path())));
}

void MainWindow::save_as() {
  if (!session_->has_project()) return;
  const QString path = QFileDialog::getSaveFileName(this, tr("Save project as"), QString(),
                                                    tr("Project (*.vsproj)"));
  if (path.isEmpty()) return;
  const auto d = session_->save(fspath(path));
  if (!d.has_errors()) add_recent(fspath(path));
  report(d, tr("Saved %1").arg(path));
}

void MainWindow::export_dialog(ExportFormat format) {
  const bool stl = format == ExportFormat::Stl;
  const QString path = QFileDialog::getSaveFileName(this, stl ? tr("Export STL") : tr("Export 3MF"), QString(),
                                                    stl ? tr("STL (*.stl)") : tr("3MF (*.3mf)"));
  if (path.isEmpty()) return;
  export_to(fspath(path), format);
}

void MainWindow::export_to(const std::filesystem::path& path, ExportFormat format,
                           std::function<void(const ExportResult&)> done) {
  const SnapshotPtr s = session_->latest();
  if (!s || s->preview || session_->busy()) {
    ExportResult r;
    r.diagnostics.add(core::Diagnostic::error("app.export.busy",
                                              "Wait for the evaluation to finish before exporting."));
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
        report(result->diagnostics,
               tr("Exported %1 (%2 triangles, %3 objects)")
                   .arg(qpath(path))
                   .arg(result->triangles)
                   .arg(result->objects));
        if (done) done(*result);
      });
}

void MainWindow::validate() {
  const SnapshotPtr s = session_->latest();
  if (!s) return;
  status_label_->setText(tr("Validating…"));
  auto report_ptr = std::make_shared<validate::ValidationReport>();
  const core::ProductSettings settings = session_->settings();
  session_->run_in_background(
      [s, report_ptr, settings] {
        *report_ptr = validate::validate(s->project->document, settings, &s->project->blobs);
      },
      [this, report_ptr] {
        show_diagnostics(report_ptr->diagnostics, tr("Validation"));
        status_label_->setText(report_ptr->ok() ? tr("Validation passed")
                                                : tr("Validation found %n error(s)", nullptr,
                                                     static_cast<int>(report_ptr->diagnostics.count(
                                                         core::Severity::Error))));
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

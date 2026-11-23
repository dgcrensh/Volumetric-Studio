#include "sketch_mode.hpp"

#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSplitter>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <optional>

#include "dimension_drag.hpp"
#include "widgets.hpp"

namespace vs::app {

namespace {

using sketch::ConstraintKind;

constexpr double kSnapPixels = 8.0;
constexpr double kPickPixels = 6.0;

const QColor kSelected(240, 140, 20);
const QColor kConflict(210, 40, 40);
const QColor kFree(40, 90, 200);
const QColor kFixed(20, 120, 60);
const QColor kConstruction(140, 140, 150);

QString glyph(ConstraintKind k) {
  switch (k) {
    case ConstraintKind::Horizontal: return QStringLiteral("H");
    case ConstraintKind::Vertical: return QStringLiteral("V");
    case ConstraintKind::Parallel: return QStringLiteral("//");
    case ConstraintKind::Perpendicular: return QStringLiteral("⊥");
    case ConstraintKind::Tangent: return QStringLiteral("T");
    case ConstraintKind::Equal: return QStringLiteral("=");
    case ConstraintKind::Symmetric: return QStringLiteral("S");
    case ConstraintKind::Fix: return QStringLiteral("F");
    case ConstraintKind::PointOnCurve: return QStringLiteral("on");
    case ConstraintKind::Coincident: return QStringLiteral("C");
    default: return {};
  }
}

QString kind_label(ConstraintKind k) {
  switch (k) {
    case ConstraintKind::Coincident: return QObject::tr("Coincident");
    case ConstraintKind::PointOnCurve: return QObject::tr("Point on curve");
    case ConstraintKind::Horizontal: return QObject::tr("Horizontal");
    case ConstraintKind::Vertical: return QObject::tr("Vertical");
    case ConstraintKind::Parallel: return QObject::tr("Parallel");
    case ConstraintKind::Perpendicular: return QObject::tr("Perpendicular");
    case ConstraintKind::Tangent: return QObject::tr("Tangent");
    case ConstraintKind::Equal: return QObject::tr("Equal");
    case ConstraintKind::Distance: return QObject::tr("Distance");
    case ConstraintKind::HorizontalDistance: return QObject::tr("Horizontal distance");
    case ConstraintKind::VerticalDistance: return QObject::tr("Vertical distance");
    case ConstraintKind::Angle: return QObject::tr("Angle");
    case ConstraintKind::Radius: return QObject::tr("Radius");
    case ConstraintKind::Diameter: return QObject::tr("Diameter");
    case ConstraintKind::Symmetric: return QObject::tr("Symmetric");
    case ConstraintKind::Fix: return QObject::tr("Fix");
  }
  return {};
}

QString dimension_text(const sketch::Constraint& c) {
  if (!c.dimension) return {};
  QString v = c.kind == ConstraintKind::Angle
                  ? QString::number(c.dimension->value * 180.0 / std::numbers::pi, 'g', 6) + QStringLiteral("°")
                  : QString::number(c.dimension->value, 'g', 6);
  if (!c.dimension->expression.empty()) return qstr(c.dimension->expression) + QStringLiteral(" = ") + v;
  return v;
}

}  // namespace

// --- the canvas ----------------------------------------------------------------------

SketchView::SketchView(SketchEditor& editor, QWidget* parent) : QWidget(parent), editor_(editor) {
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(320, 240);  // the viewport's, so sketching never widens the window
  setAutoFillBackground(false);
}

double SketchView::pixel() const { return 1.0 / scale_; }

QPointF SketchView::to_screen(dvec2 p) const { return {origin_.x() + p.x * scale_, origin_.y() - p.y * scale_}; }

dvec2 SketchView::to_sketch(QPointF p) const { return {(p.x() - origin_.x()) / scale_, (origin_.y() - p.y()) / scale_}; }

void SketchView::set_selection(std::vector<Id> ids) {
  selection_ = std::move(ids);
  emit selectionChanged();
  update();
}

void SketchView::fit() {
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  for (const auto& e : editor_.sketch().entities()) {
    if (const auto p = editor_.sketch().point(e.id)) {
      x0 = std::min(x0, p->x);
      y0 = std::min(y0, p->y);
      x1 = std::max(x1, p->x);
      y1 = std::max(y1, p->y);
    }
    if (const auto c = sketch::circle_geometry(editor_.sketch(), e.id)) {
      x0 = std::min(x0, c->center.x - c->radius);
      y0 = std::min(y0, c->center.y - c->radius);
      x1 = std::max(x1, c->center.x + c->radius);
      y1 = std::max(y1, c->center.y + c->radius);
    }
  }
  if (x0 > x1) {
    x0 = y0 = -50;
    x1 = y1 = 50;
  }
  // Keep the origin in view.
  x0 = std::min(x0, 0.0);
  y0 = std::min(y0, 0.0);
  x1 = std::max(x1, 0.0);
  y1 = std::max(y1, 0.0);
  const double w = std::max(x1 - x0, 1.0), h = std::max(y1 - y0, 1.0);
  const double usable_w = std::max(50, width() - 80), usable_h = std::max(50, height() - 80);
  scale_ = std::min(usable_w / w, usable_h / h);
  const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
  origin_ = QPointF(width() / 2.0 - cx * scale_, height() / 2.0 + cy * scale_);
  fitted_ = true;
  update();
}

QRectF SketchView::dimension_label(const Id& constraint) const {
  for (const auto& [id, box] : labels_) {
    if (id == constraint) return box;
  }
  return {};
}

void SketchView::resizeEvent(QResizeEvent*) {
  if (!fitted_ && width() > 0) fit();
}

void SketchView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(rect(), QColor(250, 250, 252));
  const sketch::Sketch& sk = editor_.sketch();

  // Grid: the smallest power-of-ten step at least 12 px apart, every fifth
  // line darker.
  double step = 0.1;
  while (step * scale_ < 12.0) step *= 10.0;
  const dvec2 lo = to_sketch(QPointF(0, height())), hi = to_sketch(QPointF(width(), 0));
  for (double x = std::floor(lo.x / step) * step; x <= hi.x; x += step) {
    const bool major = std::abs(std::remainder(x, 5 * step)) < step * 0.01;
    p.setPen(QPen(major ? QColor(214, 218, 226) : QColor(234, 236, 241), 1));
    p.drawLine(to_screen({x, lo.y}), to_screen({x, hi.y}));
  }
  for (double y = std::floor(lo.y / step) * step; y <= hi.y; y += step) {
    const bool major = std::abs(std::remainder(y, 5 * step)) < step * 0.01;
    p.setPen(QPen(major ? QColor(214, 218, 226) : QColor(234, 236, 241), 1));
    p.drawLine(to_screen({lo.x, y}), to_screen({hi.x, y}));
  }
  p.setPen(QPen(QColor(220, 90, 90), 1.2));
  p.drawLine(to_screen({lo.x, 0}), to_screen({hi.x, 0}));
  p.setPen(QPen(QColor(80, 170, 90), 1.2));
  p.drawLine(to_screen({0, lo.y}), to_screen({0, hi.y}));

  auto selected = [&](const Id& id) { return std::find(selection_.begin(), selection_.end(), id) != selection_.end(); };
  // Entities taking part in a conflicting constraint.
  std::vector<Id> conflicted;
  for (const auto& c : sk.constraints()) {
    if (editor_.is_conflicting(c.id)) conflicted.insert(conflicted.end(), c.refs.begin(), c.refs.end());
  }
  auto colour = [&](const sketch::Entity& e) {
    if (selected(e.id)) return kSelected;
    if (std::find(conflicted.begin(), conflicted.end(), e.id) != conflicted.end()) return kConflict;
    if (e.construction) return kConstruction;
    return editor_.is_free(e.id) ? kFree : kFixed;
  };

  for (const auto& e : sk.entities()) {
    if (e.kind() == sketch::EntityKind::Point) continue;
    QPen pen(colour(e), selected(e.id) ? 3.0 : 2.0);
    if (e.construction) pen.setStyle(Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    if (const auto l = sketch::line_geometry(sk, e.id)) {
      p.drawLine(to_screen(l->start), to_screen(l->end));
    } else if (const auto c = sketch::circle_geometry(sk, e.id)) {
      const QPointF centre = to_screen(c->center);
      p.drawEllipse(centre, c->radius * scale_, c->radius * scale_);
    } else if (const auto a = sketch::arc_geometry(sk, e.id)) {
      const QPointF centre = to_screen(a->center);
      const double r = a->radius * scale_;
      const QRectF box(centre.x() - r, centre.y() - r, 2 * r, 2 * r);
      const double start = a->start_angle * 180.0 / std::numbers::pi;
      const double span = (a->end_angle - a->start_angle) * 180.0 / std::numbers::pi;
      p.drawArc(box, static_cast<int>(std::lround(start * 16)), static_cast<int>(std::lround(span * 16)));
    } else if (const auto b = sketch::bezier_geometry(sk, e.id)) {
      QPainterPath path(to_screen(b->p[0]));
      path.cubicTo(to_screen(b->p[1]), to_screen(b->p[2]), to_screen(b->p[3]));
      p.drawPath(path);
      // Handles: thin lines from each end to its control point.
      QPen handle(colour(e), 1.0, Qt::DotLine);
      p.setPen(handle);
      p.drawLine(to_screen(b->p[0]), to_screen(b->p[1]));
      p.drawLine(to_screen(b->p[3]), to_screen(b->p[2]));
    }
  }
  for (const auto& e : sk.entities()) {
    const auto* pt = e.as<sketch::Point>();
    if (pt == nullptr) continue;
    const QPointF s = to_screen(pt->position);
    const double r = selected(e.id) ? 4.0 : 3.0;
    p.setPen(Qt::NoPen);
    p.setBrush(colour(e));
    p.drawRect(QRectF(s.x() - r, s.y() - r, 2 * r, 2 * r));
  }

  // Constraint glyphs and dimension labels, at the middle of what they refer to.
  auto anchor = [&](const Id& id) -> std::optional<QPointF> {
    if (const auto pt = sk.point(id)) return to_screen(*pt);
    if (const auto l = sketch::line_geometry(sk, id)) return to_screen((l->start + l->end) * 0.5);
    if (const auto c = sketch::circle_geometry(sk, id)) return to_screen(c->center + dvec2{c->radius, 0});
    if (const auto a = sketch::arc_geometry(sk, id)) return to_screen(a->at((a->start_angle + a->end_angle) / 2));
    if (const auto b = sketch::bezier_geometry(sk, id)) return to_screen(b->at(0.5));
    return std::nullopt;
  };
  QFont font = p.font();
  font.setPixelSize(11);
  p.setFont(font);
  std::map<std::pair<int, int>, int> stacked;  // keeps labels at one anchor apart
  labels_.clear();
  for (const auto& c : sk.constraints()) {
    QString text = c.dimension ? dimension_text(c) : glyph(c.kind);
    if (text.isEmpty()) continue;
    QPointF at;
    int n = 0;
    for (const auto& r : c.refs) {
      if (const auto a = anchor(r)) {
        at += *a;
        ++n;
      }
    }
    if (n == 0) continue;
    at /= n;
    auto& k = stacked[{static_cast<int>(at.x()) / 8, static_cast<int>(at.y()) / 8}];
    at += QPointF(6, -6 - 13.0 * k++);
    const bool bad = editor_.is_conflicting(c.id);
    const bool sel = selected(c.id);
    const QRectF box = p.fontMetrics().boundingRect(text).toRectF().translated(at).adjusted(-3, -1, 3, 1);
    if (c.dimension) labels_.emplace_back(c.id, box);
    p.setPen(Qt::NoPen);
    p.setBrush(c.dimension ? QColor(255, 255, 255, 230) : QColor(255, 255, 255, 170));
    p.drawRoundedRect(box, 3, 3);
    p.setPen(bad ? kConflict : sel ? kSelected : c.dimension ? QColor(30, 30, 40) : QColor(90, 100, 120));
    p.drawText(at, text);
  }

  // The shape in progress.
  std::vector<dvec2> pending = editor_.pending();
  if (pending.empty() && (editor_.tool() == SketchTool::Line || editor_.tool() == SketchTool::Bezier) &&
      !editor_.chain_end().is_null()) {
    pending.push_back(*sk.point(editor_.chain_end()));
  }
  if (!pending.empty()) {
    p.setPen(QPen(kSelected, 1.5, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    const dvec2 h = to_sketch(hover_);
    switch (editor_.tool()) {
      case SketchTool::Line: p.drawLine(to_screen(pending.back()), hover_); break;
      case SketchTool::Rectangle:
        p.drawRect(QRectF(to_screen(pending[0]), hover_).normalized());
        break;
      case SketchTool::Circle: {
        const double r = distance(pending[0], h) * scale_;
        p.drawEllipse(to_screen(pending[0]), r, r);
        break;
      }
      case SketchTool::Arc:
      case SketchTool::Slot:
        for (const auto& q : pending) p.drawLine(to_screen(q), hover_);
        break;
      case SketchTool::Bezier: {
        // The control polygon so far, and the curve once both handles are down.
        QPolygonF polygon;
        for (const auto& q : pending) polygon << to_screen(q);
        polygon << hover_;
        p.drawPolyline(polygon);
        if (pending.size() == 3) {
          QPainterPath curve(to_screen(pending[0]));
          curve.cubicTo(to_screen(pending[1]), to_screen(pending[2]), hover_);
          p.setPen(QPen(kSelected, 1.5));
          p.drawPath(curve);
        }
        break;
      }
      default: break;
    }
  }
}

void SketchView::mousePressEvent(QMouseEvent* event) {
  setFocus();
  const dvec2 at = to_sketch(event->position());
  if (event->button() == Qt::MiddleButton) {
    panning_ = true;
    pan_from_ = event->position().toPoint();
    return;
  }
  if (event->button() == Qt::RightButton) {
    editor_.finish_tool();
    update();
    return;
  }
  if (event->button() != Qt::LeftButton) return;
  // M16: a press on a dimension's label drags its value.
  if (editor_.tool() == SketchTool::Select) {
    for (const auto& [id, box] : labels_) {
      if (!box.contains(event->position())) continue;
      const auto* c = editor_.sketch().find_constraint(id);
      if (c == nullptr || !c->dimension) break;
      set_selection({id});
      if (!c->dimension->expression.empty()) {
        emit message(tr("%1 is driven by \"%2\": change that instead, or type a number to drive it by hand.")
                         .arg(qstr(editor_.sketch().label(id)), qstr(c->dimension->expression)));
        return;
      }
      if (!sketch_dimension_handle(editor_.sketch(), *c)) {
        emit message(tr("Type a new value for %1 in the list.").arg(qstr(editor_.sketch().label(id))));
        return;
      }
      dragging_dimension_ = id;
      dimension_start_ = c->dimension->value;
      dimension_from_ = event->position();
      drag_started_ = false;
      return;
    }
  }
  if (editor_.tool() != SketchTool::Select) {
    const Id snap = editor_.nearest_point(at, kSnapPixels * pixel());
    const auto added = editor_.click(at, snap);
    if (!added.empty()) emit edited();
    update();
    return;
  }
  const Id id = editor_.nearest_entity(at, kPickPixels * pixel());
  std::vector<Id> sel = selection_;
  if (event->modifiers() & Qt::ControlModifier) {
    if (!id.is_null()) {
      const auto it = std::find(sel.begin(), sel.end(), id);
      if (it == sel.end()) {
        sel.push_back(id);
      } else {
        sel.erase(it);
      }
    }
  } else {
    sel.clear();
    if (!id.is_null()) sel.push_back(id);
  }
  set_selection(std::move(sel));
  if (!id.is_null() && editor_.sketch().point(id)) {
    dragging_ = id;
    drag_started_ = false;
  }
}

void SketchView::mouseMoveEvent(QMouseEvent* event) {
  hover_ = event->position();
  if (panning_) {
    const QPoint now = event->position().toPoint();
    origin_ += QPointF(now - pan_from_);
    pan_from_ = now;
  } else if (!dragging_dimension_.is_null() && (event->buttons() & Qt::LeftButton)) {
    const auto* c = editor_.sketch().find_constraint(dragging_dimension_);
    const auto h = c != nullptr ? sketch_dimension_handle(editor_.sketch(), *c) : std::nullopt;
    if (h) {
      // The handle's direction as it was at the press: a drag measures
      // from where it started.
      const dvec2 moved = to_sketch(event->position()) - to_sketch(dimension_from_);
      // SketchEditor::drag_dimension() keeps unsigned dimensions positive (H5).
      const double value = dimension_start_ + h->rate * (moved.x * h->direction.x + moved.y * h->direction.y);
      editor_.drag_dimension(dragging_dimension_, value, !drag_started_);
      drag_started_ = true;
    }
  } else if (!dragging_.is_null() && (event->buttons() & Qt::LeftButton)) {
    editor_.drag(dragging_, to_sketch(event->position()), !drag_started_);
    drag_started_ = true;
  }
  update();
}

void SketchView::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::MiddleButton) panning_ = false;
  if (event->button() == Qt::LeftButton && !dragging_dimension_.is_null()) {
    if (drag_started_) emit edited();
    dragging_dimension_ = {};
    drag_started_ = false;
  }
  if (event->button() == Qt::LeftButton && !dragging_.is_null()) {
    if (drag_started_) emit edited();
    dragging_ = {};
    drag_started_ = false;
  }
}

void SketchView::wheelEvent(QWheelEvent* event) {
  const double factor = std::pow(1.0015, event->angleDelta().y());
  const QPointF at = event->position();
  const dvec2 before = to_sketch(at);
  scale_ = std::clamp(scale_ * factor, 0.01, 1e5);
  origin_ = QPointF(at.x() - before.x * scale_, at.y() + before.y * scale_);
  update();
}

void SketchView::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) {
    if (!editor_.pending().empty() || !editor_.chain_end().is_null()) {
      editor_.finish_tool();
    } else {
      set_selection({});
    }
    update();
    return;
  }
  if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
    if (editor_.remove(selection_)) {
      selection_.clear();
      emit selectionChanged();
      emit edited();
    }
    update();
    return;
  }
  QWidget::keyPressEvent(event);
}

// --- the panel -----------------------------------------------------------------------------

SketchMode::SketchMode(std::unique_ptr<SketchEditor> editor, const QString& title, QWidget* parent)
    : QWidget(parent), editor_(std::move(editor)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  view_ = new SketchView(*editor_);
  layout->addWidget(view_, 1);

  // Undo and redo of the sketch's own steps, with the keys while sketch
  // mode has focus (the window's are set aside meanwhile, H7).
  undo_ = new QAction(tr("Undo"), this);
  undo_->setShortcut(QKeySequence::Undo);
  undo_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  connect(undo_, &QAction::triggered, this, &SketchMode::undo);
  redo_ = new QAction(tr("Redo"), this);
  redo_->setShortcut(QKeySequence::Redo);
  redo_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  connect(redo_, &QAction::triggered, this, &SketchMode::redo);
  addAction(undo_);
  addAction(redo_);

  // The side panel: parentless until the window docks it; deleted with us.
  side_ = new QWidget;
  side_->setObjectName(QStringLiteral("sketch_panel"));
  auto* sv = new QVBoxLayout(side_);
  sv->setContentsMargins(4, 4, 4, 4);
  auto* heading = new QLabel(title);
  QFont hf = heading->font();
  hf.setBold(true);
  heading->setFont(hf);
  heading->setWordWrap(true);
  sv->addWidget(heading);
  status_ = new QLabel;
  status_->setWordWrap(true);
  QFont sf = status_->font();
  sf.setBold(true);
  status_->setFont(sf);
  sv->addWidget(status_);
  constraints_ = new QTreeWidget;
  constraints_->setHeaderLabels({tr("Constraint"), tr("Kind"), tr("Value")});
  constraints_->setRootIsDecorated(false);
  constraints_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  sv->addWidget(constraints_, 1);
  hint_ = new QLabel(tr("Double-click a value to edit it: a number (\"25\", \"30 deg\") or an expression "
                        "over the variables (\"width / 2\")."));
  hint_->setWordWrap(true);
  hint_->setStyleSheet(QStringLiteral("color: #666;"));
  sv->addWidget(hint_);

  connect(view_, &SketchView::edited, this, &SketchMode::refresh);
  connect(view_, &SketchView::message, this, [this](const QString& text) { status_->setText(text); });
  connect(view_, &SketchView::selectionChanged, this, [this] {
    update_actions();
    // Select the constraint rows the canvas selected.
    refreshing_ = true;
    for (int i = 0; i < constraints_->topLevelItemCount(); ++i) {
      auto* item = constraints_->topLevelItem(i);
      const auto id = Id::parse(sstr(item->data(0, Qt::UserRole).toString()));
      const auto& sel = view_->selection();
      item->setSelected(id && std::find(sel.begin(), sel.end(), *id) != sel.end());
    }
    refreshing_ = false;
  });
  connect(constraints_, &QTreeWidget::itemSelectionChanged, this, [this] {
    if (refreshing_) return;
    std::vector<Id> sel;
    for (auto* item : constraints_->selectedItems()) {
      if (const auto id = Id::parse(sstr(item->data(0, Qt::UserRole).toString()))) sel.push_back(*id);
    }
    view_->set_selection(std::move(sel));
  });
  connect(constraints_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
    if (refreshing_ || column != 2) return;
    const auto id = Id::parse(sstr(item->data(0, Qt::UserRole).toString()));
    if (id) set_dimension_text(*id, item->text(2));
  });
  set_tool(SketchTool::Select);
  refresh();
}

SketchMode::~SketchMode() { delete side_.data(); }

QString SketchMode::status_text() const { return status_->text(); }

void SketchMode::set_tool(SketchTool tool) {
  editor_->set_tool(tool);
  view_->setCursor(tool == SketchTool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
  view_->update();
  emit toolChanged();
}

bool SketchMode::constrain(ConstraintKind kind) {
  const Id id = editor_->constrain(kind, view_->selection());
  if (id.is_null()) return false;
  view_->set_selection({id});
  refresh();
  return true;
}

bool SketchMode::set_dimension_text(const Id& constraint, const QString& text) {
  const auto* c = editor_->sketch().find_constraint(constraint);
  if (c == nullptr || !c->dimension) return false;
  const auto param = parse_param(text, c->kind == ConstraintKind::Angle ? ParamUnit::Angle : ParamUnit::Length);
  bool ok = false;
  if (param) ok = editor_->set_dimension(constraint, param->value, param->expression).empty();
  refresh();
  // After refresh(), which shows the solver's status: the reason stays (H7).
  if (!ok) status_->setText(tr("Can't use \"%1\" as a dimension.").arg(text));
  return ok;
}

void SketchMode::undo() {
  editor_->undo();
  view_->set_selection({});
  refresh();
}

void SketchMode::redo() {
  editor_->redo();
  view_->set_selection({});
  refresh();
}

void SketchMode::remove_selection() {
  if (editor_->remove(view_->selection())) view_->set_selection({});
  refresh();
}

void SketchMode::toggle_construction() {
  editor_->toggle_construction(view_->selection());
  refresh();
}

void SketchMode::finish(bool accept) { emit finished(accept); }

void SketchMode::refresh() {
  refreshing_ = true;
  const sketch::Sketch& sk = editor_->sketch();
  constraints_->clear();
  for (const auto& c : sk.constraints()) {
    auto* item = new QTreeWidgetItem(constraints_);
    item->setText(0, qstr(sk.label(c.id)));
    item->setText(1, kind_label(c.kind));
    item->setData(0, Qt::UserRole, qstr(c.id.to_string()));
    if (c.dimension) {
      item->setText(2, c.dimension->expression.empty()
                           ? (c.kind == ConstraintKind::Angle
                                  ? QString::number(c.dimension->value * 180.0 / std::numbers::pi, 'g', 10) +
                                        QStringLiteral(" deg")
                                  : QString::number(c.dimension->value, 'g', 10))
                           : qstr(c.dimension->expression));
      item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    if (editor_->is_conflicting(c.id)) {
      for (int col = 0; col < 3; ++col) item->setForeground(col, kConflict);
    }
  }
  status_->setText(qstr(editor_->status_text()));
  QPalette pal = status_->palette();
  pal.setColor(QPalette::WindowText, editor_->status() == sketch::SolveStatus::OverConstrained ||
                                             editor_->status() == sketch::SolveStatus::Failed
                                         ? kConflict
                                         : editor_->status() == sketch::SolveStatus::Solved ? kFixed : kFree);
  status_->setPalette(pal);
  refreshing_ = false;
  update_actions();
  view_->update();
}

void SketchMode::update_actions() {
  undo_->setEnabled(editor_->can_undo());
  redo_->setEnabled(editor_->can_redo());
}

core::Result<sketch::SvgImport> SketchMode::preview_import(std::string_view svg,
                                                          const sketch::SvgImportOptions& options) {
  if (import_previewing_) editor_->undo_and_forget();
  auto imported = editor_->import_svg(svg, options);
  import_previewing_ = imported.ok();
  if (import_previewing_) view_->fit();
  refresh();
  emit view_->edited();
  return imported;
}

void SketchMode::keep_import() { import_previewing_ = false; }

void SketchMode::drop_import() {
  if (!import_previewing_) return;
  import_previewing_ = false;
  editor_->undo_and_forget();
  refresh();
  emit view_->edited();
}

}  // namespace vs::app

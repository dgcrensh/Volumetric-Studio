#include "node_editor.hpp"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QFormLayout>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneDragDropEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsTextItem>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMetaObject>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QSplitter>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QResizeEvent>
#include <QWheelEvent>

#include <fmt/format.h>

#include <algorithm>
#include <cmath>

#include "authoring.hpp"
#include "vs/proc/check.hpp"
#include "vs/proc/templates.hpp"

namespace vs::app {

namespace {

constexpr double kNodeWidth = 190.0;
constexpr double kHeader = 26.0;
constexpr double kRow = 20.0;
constexpr double kPortRadius = 5.5;
const QString kMimePrefix = QStringLiteral("vsnode:");

QColor qcol(const Rgb& c) { return QColor::fromRgbF(c.r, c.g, c.b); }

// The palette's order: inputs first, the output last.
std::vector<const proc::NodeType*> palette_order() {
  static const std::vector<std::string> kOrder{"input",   "query",   "field",   "math", "logic",   "vector",
                                               "list",    "frames",  "scatter", "pattern", "mesh", "convert",
                                               "output"};
  auto rank = [](const std::string& c) {
    const auto it = std::find(kOrder.begin(), kOrder.end(), c);
    return static_cast<std::size_t>(it - kOrder.begin());
  };
  auto types = proc::library().types();
  std::stable_sort(types.begin(), types.end(),
                   [&](const proc::NodeType* a, const proc::NodeType* b) { return rank(a->category) < rank(b->category); });
  return types;
}

QString num(double v) { return QString::number(v, 'g', 10); }

// V1: what a BoundaryFace property shows when it names no face.
QString whole_boundary_text() { return QStringLiteral("(whole boundary)"); }

}  // namespace

// --- settings as text ------------------------------------------------------------------------

QString setting_text(const proc::Setting& value, const GraphContext& context) {
  if (const auto* d = std::get_if<double>(&value)) return num(*d);
  if (const auto* v = std::get_if<core::dvec3>(&value)) {
    return QStringLiteral("%1, %2, %3").arg(num(v->x), num(v->y), num(v->z));
  }
  const auto& s = std::get<std::string>(value);
  if (const auto id = Id::parse(s)) {
    for (const auto& surface : context.surfaces) {
      if (surface.id == *id) return qstr(surface.name);
    }
  }
  // V1: a boundary face by its label.
  for (const auto& face : context.boundary_faces) {
    if (face.name == s) return qstr(face.label);
  }
  return qstr(s);
}

std::optional<proc::Setting> parse_setting(const QString& text, const proc::Setting& like, const GraphContext& context) {
  const QString t = text.trimmed();
  if (std::holds_alternative<double>(like)) {
    bool ok = false;
    const double v = t.toDouble(&ok);
    if (!ok || !std::isfinite(v)) return std::nullopt;
    return proc::Setting(v);
  }
  if (std::holds_alternative<core::dvec3>(like)) {
    QString inner = t;
    inner.remove(QLatin1Char('(')).remove(QLatin1Char(')'));
    const QStringList parts = inner.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    if (parts.size() != 3) return std::nullopt;
    double xyz[3];
    for (int i = 0; i < 3; ++i) {
      bool ok = false;
      xyz[i] = parts[i].toDouble(&ok);
      if (!ok || !std::isfinite(xyz[i])) return std::nullopt;
    }
    return proc::Setting(core::dvec3{xyz[0], xyz[1], xyz[2]});
  }
  for (const auto& surface : context.surfaces) {
    if (qstr(surface.name) == t) return proc::Setting(surface.id.to_string());
  }
  // V1: a boundary face's label, or the whole boundary (no name).
  if (t == whole_boundary_text()) return proc::Setting(std::string());
  for (const auto& face : context.boundary_faces) {
    if (qstr(face.label) == t) return proc::Setting(face.name);
  }
  return proc::Setting(sstr(t));
}

// --- canvas items -------------------------------------------------------------------------------

class EdgeItem : public QGraphicsPathItem {
 public:
  EdgeItem(const proc::Edge& edge, proc::PortType type) : edge_(edge), type_(type) {
    setZValue(-1);
    setFlag(QGraphicsItem::ItemIsSelectable);
    setAcceptHoverEvents(true);
  }
  [[nodiscard]] const proc::Edge& edge() const { return edge_; }
  void route(QPointF a, QPointF b) {
    QPainterPath p(a);
    const double dx = std::max(40.0, std::abs(b.x() - a.x()) * 0.5);
    p.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
    setPath(p);
  }
  void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
    QColor c = qcol(port_color(type_));
    QPen pen(isSelected() ? QColor(255, 200, 40) : c, isSelected() ? 3.5 : 2.2);
    pen.setCapStyle(Qt::RoundCap);
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path());
  }
  [[nodiscard]] QPainterPath shape() const override {
    QPainterPathStroker s;
    s.setWidth(10);
    return s.createStroke(path());
  }

 private:
  proc::Edge edge_;
  proc::PortType type_;
};

class NodeItem : public QGraphicsItem {
 public:
  struct Port {
    std::string name;
    proc::PortType type = proc::PortType::Scalar;
    QString value;  // an unconnected Scalar/Vector input's value
    bool connected = false;
    bool marked = false;
  };

  NodeItem(NodeEditor* editor, const proc::Node& node, const proc::NodeType* type) : editor_(editor), id_(node.id) {
    setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable |
             QGraphicsItem::ItemSendsGeometryChanges);
    title_ = qstr(node.name.empty() ? node_title(node.type) : node.name);
    type_name_ = qstr(node.type);
    known_ = type != nullptr;
    if (type != nullptr) {
      for (const auto& p : type->inputs) {
        Port port{p.name, p.type, {}, false, false};
        if (p.type == proc::PortType::Scalar || p.type == proc::PortType::Vector) {
          if (const auto v = input_value(node, p)) port.value = setting_text(*v);
        } else if (p.optional) {
          port.value = QStringLiteral("optional");
        }
        inputs_.push_back(std::move(port));
      }
      for (const auto& p : type->outputs) outputs_.push_back({p.name, p.type, {}, false, false});
      header_ = type->outputs.empty() ? QColor(70, 74, 82) : qcol(port_color(type->outputs.front().type)).darker(115);
      for (const auto& p : type->properties) {
        if (const proc::Setting* s = node.find_setting(p.name)) {
          properties_ += (properties_.isEmpty() ? QString() : QStringLiteral(", ")) +
                         setting_text(*s, editor->context());
        }
      }
    } else {
      header_ = QColor(160, 40, 40);
    }
  }

  [[nodiscard]] const Id& id() const { return id_; }
  std::vector<Port>& inputs() { return inputs_; }
  std::vector<Port>& outputs() { return outputs_; }

  void set_mark(const NodeMark* mark) {
    mark_ = mark != nullptr ? std::optional<NodeMark>(*mark) : std::nullopt;
    for (auto& p : inputs_) {
      p.marked = mark_ && std::find(mark_->ports.begin(), mark_->ports.end(), p.name) != mark_->ports.end();
    }
    delete message_;
    message_ = nullptr;
    if (mark_ && mark_->severity != core::Severity::Info && !mark_->messages.empty()) {
      message_ = new QGraphicsTextItem(this);
      message_->setTextWidth(kNodeWidth + 40);
      message_->setDefaultTextColor(mark_->failed() ? QColor(190, 30, 30) : QColor(170, 110, 0));
      QString text;
      for (const auto& m : mark_->messages) text += (text.isEmpty() ? QString() : QStringLiteral("\n")) + qstr(m);
      message_->setPlainText(text);
      QFont f = message_->font();
      f.setPointSizeF(f.pointSizeF() * 0.85);
      message_->setFont(f);
      message_->setPos(-4, height() + 2);
    }
    setToolTip(mark_ ? qstr(mark_->code) : type_name_);
    update();
  }
  void set_previewed(bool on) {
    previewed_ = on;
    update();
  }
  // While a wire of `type` is drawn from an output (`from_output`) or an
  // input: which ports could take it. nullopt: no wire.
  void set_wire(std::optional<proc::PortType> type, bool from_output) {
    wire_ = type;
    wire_from_output_ = from_output;
    update();
  }

  [[nodiscard]] double height() const {
    const std::size_t rows = std::max(inputs_.size(), outputs_.size());
    return kHeader + static_cast<double>(rows) * kRow + (properties_.isEmpty() ? 6.0 : kRow + 2.0);
  }
  [[nodiscard]] QRectF boundingRect() const override {
    return QRectF(-kPortRadius - 3, -3, kNodeWidth + 2 * kPortRadius + 6, height() + 6);
  }
  [[nodiscard]] QPointF port_point(bool input, std::size_t index) const {
    return QPointF(input ? 0.0 : kNodeWidth, kHeader + (static_cast<double>(index) + 0.5) * kRow);
  }
  [[nodiscard]] QPointF port_scene_point(bool input, const std::string& name) const {
    const auto& ports = input ? inputs_ : outputs_;
    for (std::size_t i = 0; i < ports.size(); ++i) {
      if (ports[i].name == name) return mapToScene(port_point(input, i));
    }
    return mapToScene(QPointF(input ? 0.0 : kNodeWidth, kHeader / 2));
  }
  struct Hit {
    bool input = false;
    std::size_t index = 0;
  };
  [[nodiscard]] std::optional<Hit> port_at(QPointF local) const {
    for (int side = 0; side < 2; ++side) {
      const bool input = side == 0;
      const auto& ports = input ? inputs_ : outputs_;
      for (std::size_t i = 0; i < ports.size(); ++i) {
        const QPointF p = port_point(input, i);
        if (std::hypot(local.x() - p.x(), local.y() - p.y()) <= kPortRadius + 4) return Hit{input, i};
      }
    }
    return std::nullopt;
  }

  void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
    painter->setRenderHint(QPainter::Antialiasing);
    const QRectF body(0, 0, kNodeWidth, height());
    QColor outline(90, 95, 105);
    double width = 1.2;
    if (mark_ && mark_->failed()) {
      outline = QColor(210, 40, 40);
      width = 2.6;
    } else if (mark_ && mark_->severity == core::Severity::Warning) {
      outline = QColor(225, 150, 20);
      width = 2.2;
    }
    if (isSelected()) {
      outline = QColor(255, 196, 30);
      width = 3.0;
    }
    painter->setPen(QPen(outline, width));
    painter->setBrush(QColor(250, 251, 253));
    painter->drawRoundedRect(body, 7, 7);
    // Header.
    {
      QPainterPath clip;
      clip.addRoundedRect(body.adjusted(1, 1, -1, 0), 6, 6);
      painter->save();
      painter->setClipPath(clip);
      painter->fillRect(QRectF(0, 0, kNodeWidth, kHeader), header_);
      painter->restore();
    }
    painter->setPen(Qt::white);
    QFont bold = painter->font();
    bold.setBold(true);
    painter->setFont(bold);
    QString title = title_;
    if (previewed_) title = QStringLiteral("◉ ") + title;
    painter->drawText(QRectF(8, 0, kNodeWidth - 16, kHeader), Qt::AlignVCenter | Qt::AlignLeft,
                      painter->fontMetrics().elidedText(title, Qt::ElideRight, static_cast<int>(kNodeWidth - 16)));
    QFont normal = bold;
    normal.setBold(false);
    painter->setFont(normal);
    // Ports.
    auto draw_port = [&](bool input, std::size_t i, const Port& p) {
      const QPointF c = port_point(input, i);
      QColor fill = qcol(port_color(p.type));
      bool lit = true;
      if (wire_) lit = p.type == *wire_ && input == wire_from_output_;
      if (!lit) fill = QColor(215, 215, 220);
      painter->setPen(QPen(p.marked ? QColor(210, 40, 40) : fill.darker(150), p.marked ? 2.6 : 1.2));
      painter->setBrush((input && !p.connected && p.value.isEmpty()) ? QBrush(Qt::white) : QBrush(fill));
      const double r = (wire_ && lit) ? kPortRadius + 1.5 : kPortRadius;
      painter->drawEllipse(c, r, r);
      painter->setPen(QColor(40, 44, 52));
      QString label = qstr(p.name);
      if (input && !p.connected && !p.value.isEmpty()) {
        painter->setPen(QColor(40, 44, 52));
        const QRectF rect(c.x() + 10, c.y() - kRow / 2, kNodeWidth / 2 + 30, kRow);
        painter->drawText(rect, Qt::AlignVCenter | Qt::AlignLeft,
                          painter->fontMetrics().elidedText(label + QStringLiteral(" = ") + p.value, Qt::ElideRight,
                                                            static_cast<int>(rect.width())));
        return;
      }
      const QRectF rect = input ? QRectF(c.x() + 10, c.y() - kRow / 2, kNodeWidth / 2, kRow)
                                : QRectF(c.x() - 10 - kNodeWidth / 2, c.y() - kRow / 2, kNodeWidth / 2, kRow);
      painter->drawText(rect, Qt::AlignVCenter | (input ? Qt::AlignLeft : Qt::AlignRight), label);
    };
    for (std::size_t i = 0; i < inputs_.size(); ++i) draw_port(true, i, inputs_[i]);
    for (std::size_t i = 0; i < outputs_.size(); ++i) draw_port(false, i, outputs_[i]);
    if (!properties_.isEmpty()) {
      painter->setPen(QColor(95, 100, 112));
      const double y = kHeader + static_cast<double>(std::max(inputs_.size(), outputs_.size())) * kRow;
      painter->drawText(QRectF(8, y, kNodeWidth - 16, kRow), Qt::AlignVCenter | Qt::AlignLeft,
                        painter->fontMetrics().elidedText(properties_, Qt::ElideRight, static_cast<int>(kNodeWidth - 16)));
    }
    if (!known_) {
      painter->setPen(QColor(190, 30, 30));
      painter->drawText(QRectF(8, kHeader, kNodeWidth - 16, kRow), Qt::AlignVCenter, type_name_);
    }
  }

 protected:
  QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

 private:
  NodeEditor* editor_;
  Id id_;
  QString title_;
  QString type_name_;
  QString properties_;
  bool known_ = true;
  QColor header_;
  std::vector<Port> inputs_;
  std::vector<Port> outputs_;
  std::optional<NodeMark> mark_;
  QGraphicsTextItem* message_ = nullptr;
  bool previewed_ = false;
  std::optional<proc::PortType> wire_;
  bool wire_from_output_ = true;
};

class GraphScene : public QGraphicsScene {
 public:
  explicit GraphScene(NodeEditor* editor) : QGraphicsScene(editor), editor_(editor) {
    setBackgroundBrush(QColor(236, 238, 242));
  }

  void clear_items() {
    wire_.reset();
    delete wire_item_;
    wire_item_ = nullptr;
    clear();
    nodes_.clear();
    edges_.clear();
  }
  void add_node(NodeItem* item) {
    addItem(item);
    nodes_[item->id()] = item;
  }
  void add_edge(EdgeItem* item) {
    addItem(item);
    edges_.push_back(item);
  }
  NodeItem* node(const Id& id) const {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : it->second;
  }
  const std::map<Id, NodeItem*>& nodes() const { return nodes_; }
  void route_edges(const Id& node = {}) {
    for (EdgeItem* e : edges_) {
      if (!node.is_null() && e->edge().from.node != node && e->edge().to.node != node) continue;
      NodeItem* a = this->node(e->edge().from.node);
      NodeItem* b = this->node(e->edge().to.node);
      if (a == nullptr || b == nullptr) continue;
      e->route(a->port_scene_point(false, e->edge().from.port), b->port_scene_point(true, e->edge().to.port));
    }
  }

 protected:
  void drawBackground(QPainter* painter, const QRectF& rect) override {
    QGraphicsScene::drawBackground(painter, rect);
    const double step = 24.0;
    painter->setPen(QPen(QColor(222, 225, 231), 1));
    for (double x = std::floor(rect.left() / step) * step; x < rect.right(); x += step) {
      painter->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
    }
    for (double y = std::floor(rect.top() / step) * step; y < rect.bottom(); y += step) {
      painter->drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    }
  }

  // The node item and port under a scene point.
  std::pair<NodeItem*, std::optional<NodeItem::Hit>> hit(QPointF at) const {
    for (QGraphicsItem* item : items(at)) {
      if (auto* n = dynamic_cast<NodeItem*>(item)) return {n, n->port_at(n->mapFromScene(at))};
    }
    // Ports stick out of the node's body a little.
    for (const auto& [id, n] : nodes_) {
      if (auto h = n->port_at(n->mapFromScene(at))) return {n, h};
    }
    return {nullptr, std::nullopt};
  }

  void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) {
      const auto [n, h] = hit(event->scenePos());
      if (n != nullptr && h) {
        const auto& port = h->input ? n->inputs()[h->index] : n->outputs()[h->index];
        wire_ = Wire{{n->id(), port.name}, !h->input, port.type};
        wire_item_ = new QGraphicsPathItem;
        wire_item_->setPen(QPen(qcol(port_color(port.type)), 2.2, Qt::DashLine));
        wire_item_->setZValue(10);
        addItem(wire_item_);
        wire_start_ = n->port_scene_point(h->input, port.name);
        for (const auto& [id, item] : nodes_) item->set_wire(port.type, !h->input);
        event->accept();
        return;
      }
    }
    QGraphicsScene::mousePressEvent(event);
  }
  void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
    if (wire_ && wire_item_ != nullptr) {
      QPointF a = wire_start_, b = event->scenePos();
      if (!wire_->from_output) std::swap(a, b);
      QPainterPath p(a);
      const double dx = std::max(40.0, std::abs(b.x() - a.x()) * 0.5);
      p.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
      wire_item_->setPath(p);
      return;
    }
    QGraphicsScene::mouseMoveEvent(event);
  }
  void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
    if (wire_) {
      const Wire w = *wire_;
      wire_.reset();
      delete wire_item_;
      wire_item_ = nullptr;
      for (const auto& [id, item] : nodes_) item->set_wire(std::nullopt, true);
      const auto [n, h] = hit(event->scenePos());
      if (n != nullptr && h && h->input == w.from_output && n->id() != w.port.node) {
        const auto& port = h->input ? n->inputs()[h->index] : n->outputs()[h->index];
        const proc::PortRef other{n->id(), port.name};
        NodeEditor* editor = editor_;
        const proc::PortRef from = w.port;
        const bool from_output = w.from_output;
        // After this event: the change rebuilds the scene. The drag says
        // which end is the output, for same-named ports (H5).
        QMetaObject::invokeMethod(editor, [editor, from, other, from_output] {
          editor->connect_ports(from, other, from_output);
        },
                                  Qt::QueuedConnection);
      }
      return;
    }
    QGraphicsScene::mouseReleaseEvent(event);
    // Moved nodes keep their place (itemChange records it).
  }
  void keyPressEvent(QKeyEvent* event) override {
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
      NodeEditor* editor = editor_;
      QMetaObject::invokeMethod(editor, [editor] { editor->delete_selection(); }, Qt::QueuedConnection);
      return;
    }
    QGraphicsScene::keyPressEvent(event);
  }
  void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override {
    for (QGraphicsItem* item : items(event->scenePos())) {
      if (auto* n = dynamic_cast<NodeItem*>(item)) {
        const Id id = n->id();
        const QPoint screen = event->screenPos();
        NodeEditor* editor = editor_;
        QMetaObject::invokeMethod(editor, [editor, id, screen] { editor->node_context_menu(id, screen); },
                                  Qt::QueuedConnection);
        return;
      }
      if (auto* e = dynamic_cast<EdgeItem*>(item)) {
        const proc::Edge edge = e->edge();
        // Parented, so it is styled and placed with the editor's window (H7).
        QMenu menu(editor_);
        QAction* del = menu.addAction(QObject::tr("Delete connection"));
        if (menu.exec(event->screenPos()) == del) {
          NodeEditor* editor = editor_;
          QMetaObject::invokeMethod(editor, [editor, edge] { editor->disconnect(edge); }, Qt::QueuedConnection);
        }
        return;
      }
    }
    const QPointF at = event->scenePos();
    const QPoint screen = event->screenPos();
    NodeEditor* editor = editor_;
    QMetaObject::invokeMethod(editor, [editor, at, screen] { editor->canvas_context_menu(at, screen); },
                              Qt::QueuedConnection);
  }
  void dragEnterEvent(QGraphicsSceneDragDropEvent* event) override {
    if (event->mimeData()->text().startsWith(kMimePrefix)) {
      event->acceptProposedAction();
    } else {
      event->ignore();
    }
  }
  void dragMoveEvent(QGraphicsSceneDragDropEvent* event) override {
    if (event->mimeData()->text().startsWith(kMimePrefix)) event->acceptProposedAction();
  }
  void dropEvent(QGraphicsSceneDragDropEvent* event) override {
    const QString text = event->mimeData()->text();
    if (!text.startsWith(kMimePrefix)) return;
    const std::string type = sstr(text.mid(kMimePrefix.size()));
    const QPointF at = event->scenePos();
    NodeEditor* editor = editor_;
    QMetaObject::invokeMethod(editor, [editor, type, at] { editor->add_node(type, at); }, Qt::QueuedConnection);
    event->acceptProposedAction();
  }

 private:
  struct Wire {
    proc::PortRef port;
    bool from_output = true;
    proc::PortType type = proc::PortType::Scalar;
  };
  NodeEditor* editor_;
  std::map<Id, NodeItem*> nodes_;
  std::vector<EdgeItem*> edges_;
  std::optional<Wire> wire_;
  QGraphicsPathItem* wire_item_ = nullptr;
  QPointF wire_start_;
};

QVariant NodeItem::itemChange(GraphicsItemChange change, const QVariant& value) {
  if (change == ItemPositionHasChanged && !editor_->rebuilding_) {
    editor_->positions_[id_] = pos();
    editor_->scene_->route_edges(id_);
  }
  if (change == ItemSelectedHasChanged && !editor_->rebuilding_ && value.toBool()) {
    emit editor_->nodeSelected(editor_->volume_, id_);
  }
  return QGraphicsItem::itemChange(change, value);
}

namespace {

// The palette: drags carry the node type.
class PaletteTree : public QTreeWidget {
 public:
  using QTreeWidget::QTreeWidget;

 protected:
  QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
    auto* mime = new QMimeData;
    if (!items.isEmpty()) mime->setText(kMimePrefix + items.front()->data(0, Qt::UserRole).toString());
    return mime;
  }
  QStringList mimeTypes() const override { return {QStringLiteral("text/plain")}; }
};

class CanvasView : public QGraphicsView {
 public:
  using QGraphicsView::QGraphicsView;
  // Frames the graph: whole, no bigger than life size; past half size, its top left.
  void fit() {
    auto_fit = true;
    const QRectF r = scene()->itemsBoundingRect().adjusted(-30, -30, 30, 30);
    if (r.isEmpty() || viewport()->width() < 50 || viewport()->height() < 50) return;
    const double want = std::min(viewport()->width() / r.width(), viewport()->height() / r.height());
    const double s = std::clamp(want, 0.55, 1.0);
    setTransform(QTransform::fromScale(s, s));
    if (want >= s) {
      centerOn(r.center());
    } else {
      // Too big to read whole: show its top left (the inputs).
      centerOn(r.left() + viewport()->width() / (2 * s), r.top() + viewport()->height() / (2 * s));
    }
  }
  bool auto_fit = true;  // until the user zooms

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QGraphicsView::resizeEvent(event);
    if (auto_fit) fit();
  }
  void wheelEvent(QWheelEvent* event) override {
    auto_fit = false;
    const double f = std::pow(1.0015, event->angleDelta().y());
    const double s = transform().m11() * f;
    if (s < 0.2 || s > 3.0) return;
    scale(f, f);
  }
};

}  // namespace

// --- the editor --------------------------------------------------------------------------------

NodeEditor::NodeEditor(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);
  layout->setSpacing(4);

  auto* bar = new QHBoxLayout;
  title_ = new QLabel(tr("No graph"));
  title_->setStyleSheet(QStringLiteral("QLabel { font-weight: bold; color: #1d4f91; }"));
  bar->addWidget(title_);
  bar->addStretch(1);
  auto* templates = new QPushButton(tr("Templates…"));
  connect(templates, &QPushButton::clicked, this, &NodeEditor::templatesRequested);
  auto* bake = new QPushButton(tr("Bake"));
  bake->setToolTip(tr("Store the generated mesh as static contents, keeping the graph to unbake"));
  connect(bake, &QPushButton::clicked, this, &NodeEditor::bakeRequested);
  auto* tidy = new QPushButton(tr("Auto layout"));
  connect(tidy, &QPushButton::clicked, this, &NodeEditor::auto_layout);
  field_view_ = new QComboBox;
  field_view_->addItems({tr("Fields as surfaces"), tr("Fields as a slice")});
  slice_ = new QSlider(Qt::Horizontal);
  slice_->setRange(0, 100);
  slice_->setValue(50);
  slice_->setMaximumWidth(120);
  slice_->setToolTip(tr("Slice height"));
  slice_->setEnabled(false);
  connect(field_view_, &QComboBox::currentIndexChanged, this, [this](int i) {
    slice_->setEnabled(i == 1);
    emit previewOptionsChanged();
  });
  connect(slice_, &QSlider::sliderReleased, this, &NodeEditor::previewOptionsChanged);
  connect(slice_, &QSlider::valueChanged, this, [this] {
    if (!slice_->isSliderDown()) emit previewOptionsChanged();
  });
  auto* close = new QToolButton;
  close->setText(tr("Done"));
  connect(close, &QToolButton::clicked, this, &NodeEditor::closeRequested);
  for (QWidget* w : std::initializer_list<QWidget*>{templates, bake, tidy, field_view_, slice_, close}) bar->addWidget(w);
  layout->addLayout(bar);

  auto* split = new QSplitter(Qt::Horizontal);
  // Palette.
  auto* left = new QWidget;
  auto* ll = new QVBoxLayout(left);
  ll->setContentsMargins(0, 0, 0, 0);
  search_ = new QLineEdit;
  search_->setPlaceholderText(tr("Search nodes…"));
  search_->setClearButtonEnabled(true);
  ll->addWidget(search_);
  palette_ = new PaletteTree;
  palette_->setHeaderHidden(true);
  palette_->setDragEnabled(true);
  palette_->setDragDropMode(QAbstractItemView::DragOnly);
  palette_->setRootIsDecorated(true);
  ll->addWidget(palette_);
  split->addWidget(left);
  connect(search_, &QLineEdit::textChanged, this, &NodeEditor::set_palette_filter);
  connect(search_, &QLineEdit::returnPressed, this, [this] {
    const auto types = palette_types();
    if (types.size() == 1) add_node(types.front());
  });
  connect(palette_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
    const QString type = item->data(0, Qt::UserRole).toString();
    if (!type.isEmpty()) add_node(sstr(type));
  });

  // Canvas.
  scene_ = new GraphScene(this);
  view_ = new CanvasView(scene_);
  view_->setRenderHint(QPainter::Antialiasing);
  view_->setDragMode(QGraphicsView::RubberBandDrag);
  view_->setAcceptDrops(true);
  view_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
  view_->setMinimumHeight(220);
  split->addWidget(view_);

  // Parameters.
  auto* right = new QWidget;
  auto* rl = new QVBoxLayout(right);
  rl->setContentsMargins(0, 0, 0, 0);
  rl->addWidget(new QLabel(tr("Exposed parameters")));
  parameters_ = new QTableWidget(0, 2);
  parameters_->setHorizontalHeaderLabels({tr("Name"), tr("Default")});
  parameters_->horizontalHeader()->setStretchLastSection(true);
  parameters_->verticalHeader()->setVisible(false);
  parameters_->setToolTip(tr("A volume parameter of the same name overrides the default"));
  rl->addWidget(parameters_);
  auto* pb = new QHBoxLayout;
  auto* add_p = new QPushButton(tr("Add"));
  auto* del_p = new QPushButton(tr("Delete"));
  pb->addWidget(add_p);
  pb->addWidget(del_p);
  rl->addLayout(pb);
  split->addWidget(right);
  split->setStretchFactor(0, 0);
  split->setStretchFactor(1, 1);
  split->setStretchFactor(2, 0);
  split->setSizes({200, 700, 220});
  layout->addWidget(split, 1);
  connect(add_p, &QPushButton::clicked, this, [this] {
    std::string name = "param";
    for (int i = 2; graph_.find_parameter(name) != nullptr; ++i) name = fmt::format("param{}", i);
    add_parameter(name, 0.0);
  });
  connect(del_p, &QPushButton::clicked, this, [this] {
    const int row = parameters_->currentRow();
    if (row >= 0 && static_cast<std::size_t>(row) < graph_.parameters.size()) {
      remove_parameter(graph_.parameters[static_cast<std::size_t>(row)].name);
    }
  });
  connect(parameters_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
    if (rebuilding_) return;
    const int row = item->row();
    if (row < 0 || static_cast<std::size_t>(row) >= graph_.parameters.size()) return;
    const auto& p = graph_.parameters[static_cast<std::size_t>(row)];
    const std::string name = sstr(parameters_->item(row, 0)->text().trimmed());
    bool ok = false;
    const double value = parameters_->item(row, 1)->text().trimmed().toDouble(&ok);
    if (!ok) {
      show_message(tr("'%1' isn't a number").arg(parameters_->item(row, 1)->text()), true);
      rebuild_parameters();
      return;
    }
    if (name == p.name && value == p.value) return;
    const std::string old = p.name;
    QMetaObject::invokeMethod(this, [this, old, name, value] { set_parameter(old, name, value); },
                              Qt::QueuedConnection);
  });

  message_ = new QLabel;
  message_->setWordWrap(true);
  message_->setVisible(false);
  layout->addWidget(message_);
  preview_label_ = new QLabel;
  preview_label_->setWordWrap(true);
  preview_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  preview_label_->setStyleSheet(QStringLiteral("QLabel { color: #444; }"));
  layout->addWidget(preview_label_);

  rebuild_palette();
  setEnabled(false);
}

NodeEditor::~NodeEditor() {
  rebuilding_ = true;  // items deleted with the scene must not call back
}

void NodeEditor::show_graph(const model::Project& project, const eval::Evaluation* evaluation, const Id& volume) {
  const model::Volume* v = project.document.find_volume(volume);
  const auto g = graph_of(project.document, volume);
  if (v == nullptr || !g) {
    clear();
    return;
  }
  const bool other = volume != volume_;
  core::Diagnostics from_evaluation;
  if (evaluation != nullptr) {
    if (const auto* r = evaluation->find(volume)) from_evaluation = r->diagnostics;
  }
  core::Diagnostics diagnostics = graph_diagnostics(*g, project.document, volume, from_evaluation);
  GraphContext context = graph_context(project.document, volume, evaluation);
  if (!other && *g == graph_) {
    // Only the marks may have changed: keep the scene (and any drag in
    // progress) unless they did.
    core::Diagnostics all = diagnostics;
    for (const auto& d : preview_diagnostics_) all.add(d);
    const bool same_context = context.surfaces.size() == context_.surfaces.size() &&
                              std::equal(context.surfaces.begin(), context.surfaces.end(), context_.surfaces.begin(),
                                         [](const auto& a, const auto& b) { return a.id == b.id && a.name == b.name; }) &&
                              std::equal(context.boundary_faces.begin(), context.boundary_faces.end(),
                                         context_.boundary_faces.begin(), context_.boundary_faces.end(),
                                         [](const auto& a, const auto& b) { return a.name == b.name && a.label == b.label; });
    diagnostics_ = std::move(diagnostics);
    context_ = std::move(context);
    title_->setText(tr("Graph of %1").arg(qstr(v->name)));
    if (node_marks(graph_, all) == marks_ && same_context) return;
    rebuild_scene();
    return;
  }
  volume_ = volume;
  graph_ = *g;
  context_ = std::move(context);
  diagnostics_ = std::move(diagnostics);
  if (other) {
    positions_.clear();
    previewed_ = {};
    preview_text_.clear();
    preview_diagnostics_ = {};
  }
  title_->setText(tr("Graph of %1").arg(qstr(v->name)));
  setEnabled(true);
  rebuild_parameters();
  rebuild_scene();
  if (other) static_cast<CanvasView*>(view_)->fit();
}

void NodeEditor::clear() {
  volume_ = {};
  graph_ = {};
  context_ = {};
  diagnostics_ = {};
  preview_diagnostics_ = {};
  marks_.clear();
  previewed_ = {};
  preview_text_.clear();
  rebuilding_ = true;
  scene_->clear_items();
  rebuilding_ = false;
  parameters_->setRowCount(0);
  title_->setText(tr("No graph"));
  preview_label_->clear();
  message_->setVisible(false);
  setEnabled(false);
}

void NodeEditor::rebuild_scene() {
  const Id selected = selected_node();
  core::Diagnostics all = diagnostics_;
  for (const auto& d : preview_diagnostics_) all.add(d);
  marks_ = node_marks(graph_, all);
  rebuilding_ = true;
  scene_->clear_items();
  // Places for nodes not seen yet: the layered layout, shifted to sit
  // right of what is placed, unless nothing is.
  const auto layered = layered_layout(graph_);
  for (const auto& n : graph_.nodes) {
    if (positions_.contains(n.id)) continue;
    const auto& p = layered.at(n.id);
    positions_[n.id] = QPointF(p.x, p.y);
  }
  for (const auto& n : graph_.nodes) {
    auto* item = new NodeItem(this, n, proc::library().find(n.type));
    for (auto& port : item->inputs()) port.connected = graph_.edge_into(n.id, port.name) != nullptr;
    for (auto& port : item->outputs()) {
      port.connected = std::any_of(graph_.edges.begin(), graph_.edges.end(), [&](const proc::Edge& e) {
        return e.from.node == n.id && e.from.port == port.name;
      });
    }
    item->setPos(positions_[n.id]);
    const auto it = marks_.find(n.id);
    item->set_mark(it != marks_.end() ? &it->second : nullptr);
    item->set_previewed(n.id == previewed_);
    scene_->add_node(item);
  }
  for (const auto& e : graph_.edges) {
    proc::PortType type = proc::PortType::Scalar;
    if (const proc::Node* n = graph_.find_node(e.from.node)) {
      if (const proc::NodeType* t = proc::library().find(n->type)) {
        if (const proc::PortSpec* p = t->find_output(e.from.port)) type = p->type;
      }
    }
    scene_->add_edge(new EdgeItem(e, type));
  }
  scene_->route_edges();
  if (NodeItem* n = scene_->node(selected)) n->setSelected(true);
  scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-400, -300, 400, 300));
  rebuilding_ = false;
}

void NodeEditor::rebuild_parameters() {
  rebuilding_ = true;
  parameters_->setRowCount(0);
  for (const auto& p : graph_.parameters) {
    const int row = parameters_->rowCount();
    parameters_->insertRow(row);
    parameters_->setItem(row, 0, new QTableWidgetItem(qstr(p.name)));
    parameters_->setItem(row, 1, new QTableWidgetItem(num(p.value)));
  }
  rebuilding_ = false;
}

void NodeEditor::rebuild_palette() {
  palette_->clear();
  std::map<std::string, QTreeWidgetItem*> groups;
  for (const proc::NodeType* t : palette_order()) {
    QTreeWidgetItem*& group = groups[t->category];
    if (group == nullptr) {
      group = new QTreeWidgetItem(palette_, {qstr(t->category)});
      group->setFlags(Qt::ItemIsEnabled);
      QFont f = group->font(0);
      f.setBold(true);
      group->setFont(0, f);
    }
    auto* item = new QTreeWidgetItem(group, {qstr(node_title(t->type))});
    item->setData(0, Qt::UserRole, qstr(t->type));
    item->setToolTip(0, qstr(t->type) + (t->doc.empty() ? QString() : QStringLiteral("\n") + qstr(t->doc)));
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(t->outputs.empty() ? QColor(70, 74, 82) : qcol(port_color(t->outputs.front().type)));
    p.drawEllipse(QRectF(1, 1, 10, 10));
    item->setIcon(0, QIcon(pm));
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
  }
  palette_->expandAll();
}

void NodeEditor::set_palette_filter(const QString& text) {
  if (search_->text() != text) {
    search_->setText(text);  // calls back here
    return;
  }
  const QString needle = text.trimmed();
  for (int g = 0; g < palette_->topLevelItemCount(); ++g) {
    QTreeWidgetItem* group = palette_->topLevelItem(g);
    bool any = false;
    for (int i = 0; i < group->childCount(); ++i) {
      QTreeWidgetItem* item = group->child(i);
      const bool show = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive) ||
                        item->data(0, Qt::UserRole).toString().contains(needle, Qt::CaseInsensitive) ||
                        item->toolTip(0).contains(needle, Qt::CaseInsensitive);
      item->setHidden(!show);
      any = any || show;
    }
    group->setHidden(!any);
  }
}

std::vector<std::string> NodeEditor::palette_types() const {
  std::vector<std::string> out;
  for (int g = 0; g < palette_->topLevelItemCount(); ++g) {
    QTreeWidgetItem* group = palette_->topLevelItem(g);
    if (group->isHidden()) continue;
    for (int i = 0; i < group->childCount(); ++i) {
      if (!group->child(i)->isHidden()) out.push_back(sstr(group->child(i)->data(0, Qt::UserRole).toString()));
    }
  }
  return out;
}

void NodeEditor::show_message(const QString& text, bool error) {
  message_->setText(text);
  message_->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QStringLiteral("color: #555;"));
  message_->setVisible(!text.isEmpty());
}

QString NodeEditor::message() const { return message_->isVisible() ? message_->text() : QString(); }

QString NodeEditor::preview_text() const { return preview_text_; }

NodePreviewOptions NodeEditor::preview_options() const {
  NodePreviewOptions o;
  o.field = field_view_->currentIndex() == 1 ? FieldView::Slice : FieldView::Surface;
  o.slice = slice_->value() / 100.0;
  return o;
}

core::Diagnostics NodeEditor::commit(const GraphChange& change) {
  if (!change.ok()) {
    QString why;
    for (const auto& d : change.diagnostics) {
      if (d.is_error() && !proc::is_unfinished(d.code)) {
        why = qstr(d.message);
        break;
      }
    }
    show_message(tr("Can't %1: %2").arg(qstr(change.label).toLower(), why), true);
    core::Diagnostics out;
    for (const auto& d : change.diagnostics) {
      if (!proc::is_unfinished(d.code)) out.add(d);
    }
    return out;
  }
  if (!apply_ || change.graph == graph_) return {};  // nothing changed: no undo step
  core::Diagnostics d = apply_(set_graph(volume_, change.graph, change.label));
  if (d.has_errors()) {
    for (const auto& x : d) {
      if (x.is_error()) {
        show_message(tr("Can't %1: %2").arg(qstr(change.label).toLower(), qstr(x.message)), true);
        break;
      }
    }
  } else {
    show_message({}, false);
  }
  return d;
}

QPointF NodeEditor::free_position() const {
  // Below everything placed, at the left.
  double y = 0.0;
  bool any = false;
  for (const auto& [id, item] : scene_->nodes()) {
    y = std::max(y, item->pos().y() + item->height() + 30.0);
    any = true;
  }
  return any ? QPointF(0.0, y) : QPointF(0, 0);
}

core::Diagnostics NodeEditor::add_node(const std::string& type, std::optional<QPointF> at, Id* added) {
  if (volume_.is_null()) return {};
  const GraphChange change = app::add_node(graph_, type, context_);
  if (change.ok()) positions_[change.node] = at.value_or(free_position());
  auto d = commit(change);
  if (!d.has_errors()) {
    if (added != nullptr) *added = change.node;
    select_node(change.node);
  } else {
    positions_.erase(change.node);
  }
  return d;
}

core::Diagnostics NodeEditor::connect_ports(const proc::PortRef& a, const proc::PortRef& b,
                                            std::optional<bool> a_is_output) {
  return commit(app::connect_ports(graph_, a, b, a_is_output));
}

core::Diagnostics NodeEditor::disconnect(const proc::Edge& edge) { return commit(app::disconnect(graph_, edge)); }

core::Diagnostics NodeEditor::remove_nodes(const std::vector<Id>& nodes) {
  return commit(app::remove_nodes(graph_, nodes));
}

core::Diagnostics NodeEditor::rename_node(const Id& node, const std::string& name) {
  return commit(app::rename_node(graph_, node, name));
}

core::Diagnostics NodeEditor::set_setting(const Id& node, const std::string& key, std::optional<proc::Setting> value) {
  return commit(app::set_setting(graph_, node, key, std::move(value)));
}

core::Diagnostics NodeEditor::set_setting_text(const Id& node, const std::string& key, const QString& text) {
  const proc::Node* n = graph_.find_node(node);
  const proc::NodeType* t = n != nullptr ? proc::library().find(n->type) : nullptr;
  if (t == nullptr) return {};
  proc::Setting like = std::string();
  if (const auto* p = t->find_input(key)) {
    if (text.trimmed().isEmpty()) return set_setting(node, key, std::nullopt);
    if (p->type == proc::PortType::Vector) {
      like = core::dvec3{};
    } else {
      like = 0.0;
    }
  } else if (const auto* prop = t->find_property(key)) {
    if (prop->kind == proc::PropertyKind::Number) like = 0.0;
    if (prop->kind == proc::PropertyKind::Vector) like = core::dvec3{};
  } else {
    return {};
  }
  const auto value = parse_setting(text, like, context_);
  if (!value) {
    show_message(tr("'%1' isn't a %2").arg(text, std::holds_alternative<double>(like) ? tr("number")
                                                                                         : tr("vector (x, y, z)")),
                 true);
    core::Diagnostics d;
    d.add(core::Diagnostic::error("app.graph.setting", "not a value of the setting's kind"));
    return d;
  }
  return set_setting(node, key, *value);
}

core::Diagnostics NodeEditor::add_parameter(const std::string& name, double value) {
  return commit(app::add_parameter(graph_, name, value));
}

core::Diagnostics NodeEditor::set_parameter(const std::string& name, const std::string& new_name, double value) {
  auto d = commit(app::set_parameter(graph_, name, new_name, value));
  if (d.has_errors()) rebuild_parameters();
  return d;
}

core::Diagnostics NodeEditor::remove_parameter(const std::string& name) {
  return commit(app::remove_parameter(graph_, name));
}

core::Diagnostics NodeEditor::expose_input(const Id& node, const std::string& port, Id* reader) {
  const GraphChange change = app::expose_input(graph_, node, port);
  if (change.ok()) {
    const auto p = node_position(node).value_or(QPointF());
    positions_[change.node] = p - QPointF(240, 0);
  }
  auto d = commit(change);
  if (!d.has_errors() && reader != nullptr) *reader = change.node;
  return d;
}

void NodeEditor::delete_selection() {
  std::vector<Id> nodes;
  std::vector<proc::Edge> edges;
  for (QGraphicsItem* item : scene_->selectedItems()) {
    if (auto* n = dynamic_cast<NodeItem*>(item)) nodes.push_back(n->id());
    if (auto* e = dynamic_cast<EdgeItem*>(item)) edges.push_back(e->edge());
  }
  if (!nodes.empty()) {
    remove_nodes(nodes);
  } else if (!edges.empty()) {
    GraphChange change;
    change.graph = graph_;
    change.label = edges.size() == 1 ? "Disconnect" : "Delete connections";
    change.diagnostics = proc::edit(change.graph, [&](proc::Graph& x) {
      for (const auto& e : edges) std::erase(x.edges, e);
    });
    commit(change);
  }
}

void NodeEditor::select_node(const Id& node) {
  rebuilding_ = true;
  scene_->clearSelection();
  if (NodeItem* n = scene_->node(node)) {
    n->setSelected(true);
    view_->ensureVisible(n, 40, 40);
  }
  rebuilding_ = false;
}

Id NodeEditor::selected_node() const {
  for (QGraphicsItem* item : scene_->selectedItems()) {
    if (auto* n = dynamic_cast<NodeItem*>(item)) return n->id();
  }
  return {};
}

std::optional<QPointF> NodeEditor::node_position(const Id& node) const {
  const auto it = positions_.find(node);
  if (it == positions_.end()) return std::nullopt;
  return it->second;
}

void NodeEditor::move_node(const Id& node, QPointF at) {
  positions_[node] = at;
  if (NodeItem* n = scene_->node(node)) {
    rebuilding_ = true;
    n->setPos(at);
    rebuilding_ = false;
    scene_->route_edges(node);
  }
}

void NodeEditor::auto_layout() {
  for (const auto& [id, p] : layered_layout(graph_)) positions_[id] = QPointF(p.x, p.y);
  rebuild_scene();
  static_cast<CanvasView*>(view_)->fit();
}

void NodeEditor::show_preview(const NodePreviewResult& result) {
  if (result.volume != volume_ || graph_.find_node(result.node) == nullptr) return;
  previewed_ = result.node;
  const proc::Node* n = graph_.find_node(result.node);
  const QString name = qstr(n->name.empty() ? n->type : n->name);
  if (result.ok()) {
    preview_text_ = tr("%1.%2: %3").arg(name, qstr(result.port), qstr(result.text));
  } else {
    preview_text_ = tr("%1: %2").arg(name, qstr(result.text));
  }
  preview_text_ += tr("  (%1 ms%2)")
                       .arg(result.inspect_ms + result.preview_ms, 0, 'f', 0)
                       .arg(result.from_cache ? tr(", cached") : QString());
  preview_label_->setText(preview_text_);
  preview_diagnostics_ = result.diagnostics;
  rebuild_scene();
}

void NodeEditor::clear_preview() {
  previewed_ = {};
  preview_text_.clear();
  preview_label_->clear();
  if (!preview_diagnostics_.empty()) {
    preview_diagnostics_ = {};
    rebuild_scene();
  } else if (!volume_.is_null()) {
    for (const auto& [id, item] : scene_->nodes()) item->set_previewed(false);
  }
}

void NodeEditor::node_context_menu(const Id& node, const QPoint& screen) {
  const proc::Node* n = graph_.find_node(node);
  if (n == nullptr) return;
  const proc::NodeType* t = proc::library().find(n->type);
  QMenu menu(this);
  if (t != nullptr) {
    for (const auto& out : t->outputs) {
      const QString port = qstr(out.name);
      menu.addAction(tr("Preview %1").arg(port), this, [this, node, port] {
        emit previewRequested(node, port);
      });
    }
    menu.addSeparator();
  }
  menu.addAction(tr("Rename…"), this, [this, node] {
    const proc::Node* m = graph_.find_node(node);
    if (m == nullptr) return;
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("Rename node"), tr("Name"), QLineEdit::Normal, qstr(m->name), &ok);
    if (ok) rename_node(node, sstr(name.trimmed()));
  });
  if (t != nullptr) {
    QMenu* expose = menu.addMenu(tr("Expose as parameter"));
    for (const auto& in : t->inputs) {
      if (in.type != proc::PortType::Scalar || graph_.edge_into(node, in.name) != nullptr) continue;
      const std::string port = in.name;
      expose->addAction(qstr(port), this, [this, node, port] { expose_input(node, port); });
    }
    expose->setEnabled(!expose->isEmpty());
  }
  menu.addSeparator();
  menu.addAction(tr("Delete"), this, [this, node] { remove_nodes({node}); });
  menu.exec(screen);
}

void NodeEditor::canvas_context_menu(const QPointF& at, const QPoint& screen) {
  if (volume_.is_null()) return;
  QMenu menu(this);
  QMenu* add = menu.addMenu(tr("Add node"));
  std::map<std::string, QMenu*> groups;
  for (const proc::NodeType* t : palette_order()) {
    QMenu*& g = groups[t->category];
    if (g == nullptr) g = add->addMenu(qstr(t->category));
    const std::string type = t->type;
    QAction* a = g->addAction(qstr(node_title(type)), this, [this, type, at] { add_node(type, at); });
    a->setToolTip(qstr(t->doc));
  }
  menu.addAction(tr("Auto layout"), this, &NodeEditor::auto_layout);
  menu.exec(screen);
}

// --- the node form ---------------------------------------------------------------------------------

NodeForm::NodeForm(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  header_ = new QLabel;
  header_->setWordWrap(true);
  header_->setStyleSheet(QStringLiteral("QLabel { font-weight: bold; }"));
  layout->addWidget(header_);
  form_ = new QFormLayout;
  form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  layout->addLayout(form_);
  layout->addStretch(1);
}

void NodeForm::later(const QString& key, const QString& text) {
  const Id n = node_;
  QMetaObject::invokeMethod(this, [this, n, key, text] { emit settingEdited(n, key, text); }, Qt::QueuedConnection);
}

QWidget* NodeForm::field(const std::string& key) const {
  const auto it = fields_.find(key);
  return it == fields_.end() ? nullptr : it->second;
}

void NodeForm::edit(const std::string& key, const QString& text) {
  QWidget* w = field(key);
  if (auto* e = qobject_cast<QLineEdit*>(w)) {
    e->setText(text);
    emit e->editingFinished();
  } else if (auto* c = qobject_cast<QComboBox*>(w)) {
    const int i = c->findText(text);
    if (i >= 0) {
      c->setCurrentIndex(i);
      emit c->activated(i);
    }
  }
}

void NodeForm::show_node(const proc::Graph& graph, const Id& id, const GraphContext& context) {
  // Unchanged: keep the widgets (and what is being typed in them).
  if (id == node_ && graph == shown_ && context.surfaces.size() == surfaces_ &&
      context.boundary_faces.size() == boundary_faces_ && !fields_.empty()) {
    return;
  }
  while (form_->rowCount() > 0) {
    const QFormLayout::TakeRowResult row = form_->takeRow(0);
    for (QLayoutItem* item : {row.labelItem, row.fieldItem}) {
      if (item == nullptr) continue;
      if (QWidget* w = item->widget()) {
        // Its signals go first: a name or number field losing focus as it
        // hides (or before it is deleted) would otherwise commit its text
        // to the node shown next (H7).
        QObject::disconnect(w, nullptr, this, nullptr);
        for (QObject* child : w->findChildren<QObject*>()) QObject::disconnect(child, nullptr, this, nullptr);
        w->hide();
        w->deleteLater();
      }
      delete item;
    }
  }
  fields_.clear();
  node_ = id;
  shown_ = graph;
  surfaces_ = context.surfaces.size();
  boundary_faces_ = context.boundary_faces.size();
  const proc::Node* node = graph.find_node(id);
  if (node == nullptr) {
    header_->setText(tr("No node selected"));
    return;
  }
  const proc::NodeType* type = proc::library().find(node->type);
  header_->setText(tr("%1 (%2)").arg(qstr(node->name.empty() ? node->type : node->name), qstr(node->type)));
  if (type != nullptr && !type->doc.empty()) header_->setToolTip(qstr(type->doc));

  auto* name = new QLineEdit(qstr(node->name));
  form_->addRow(tr("Name"), name);
  fields_[".name"] = name;  // not a setting key: input.parameter has a "name" property
  connect(name, &QLineEdit::editingFinished, this, [this, name] {
    const Id n = node_;
    const QString text = name->text().trimmed();
    QMetaObject::invokeMethod(this, [this, n, text] { emit renamed(n, text); }, Qt::QueuedConnection);
  });
  if (type == nullptr) return;

  auto line = [&](const std::string& key, const QString& text, const QString& tip) {
    auto* e = new QLineEdit(text);
    e->setToolTip(tip);
    fields_[key] = e;
    const QString k = qstr(key);
    connect(e, &QLineEdit::editingFinished, this, [this, e, k] { later(k, e->text()); });
    return e;
  };
  auto combo = [&](const std::string& key, const QStringList& items, const QString& current, const QString& tip) {
    auto* c = new QComboBox;
    c->addItems(items);
    c->setCurrentText(current);
    c->setToolTip(tip);
    fields_[key] = c;
    const QString k = qstr(key);
    connect(c, &QComboBox::activated, this, [this, c, k](int) { later(k, c->currentText()); });
    return c;
  };

  for (const auto& p : type->inputs) {
    const QString label = qstr(p.name);
    if (const proc::Edge* e = graph.edge_into(id, p.name)) {
      const proc::Node* from = graph.find_node(e->from.node);
      auto* l = new QLabel(tr("from %1.%2").arg(from != nullptr ? qstr(from->name) : tr("?"), qstr(e->from.port)));
      l->setStyleSheet(QStringLiteral("color: #666;"));
      form_->addRow(label, l);
      continue;
    }
    if (p.type != proc::PortType::Scalar && p.type != proc::PortType::Vector) {
      auto* l = new QLabel(p.optional ? tr("(optional %1, not connected)").arg(qstr(proc::to_string(p.type)))
                                      : tr("(%1: connect one)").arg(qstr(proc::to_string(p.type))));
      l->setStyleSheet(p.optional ? QStringLiteral("color: #666;") : QStringLiteral("color: #b06e00;"));
      form_->addRow(label, l);
      continue;
    }
    const auto v = input_value(*node, p);
    QLineEdit* e = line(p.name, v ? setting_text(*v) : QString(), qstr(p.doc));
    if (p.default_value) e->setPlaceholderText(setting_text(*p.default_value));
    if (p.type == proc::PortType::Scalar) {
      auto* row = new QWidget;
      auto* h = new QHBoxLayout(row);
      h->setContentsMargins(0, 0, 0, 0);
      h->addWidget(e, 1);
      auto* expose = new QToolButton;
      expose->setText(tr("Expose"));
      expose->setToolTip(tr("Make this a parameter of the graph"));
      const QString port = qstr(p.name);
      connect(expose, &QToolButton::clicked, this, [this, port] {
        const Id n = node_;
        QMetaObject::invokeMethod(this, [this, n, port] { emit exposeRequested(n, port); }, Qt::QueuedConnection);
      });
      h->addWidget(expose);
      form_->addRow(label, row);
    } else {
      form_->addRow(label, e);
    }
  }
  for (const auto& p : type->properties) {
    const proc::Setting* s = node->find_setting(p.name);
    const QString current = s != nullptr ? setting_text(*s, context)
                                         : (p.default_value ? setting_text(*p.default_value, context) : QString());
    QWidget* w = nullptr;
    switch (p.kind) {
      case proc::PropertyKind::Choice: {
        QStringList items;
        for (const auto& c : p.choices) items << qstr(c);
        w = combo(p.name, items, current, qstr(p.doc));
        break;
      }
      case proc::PropertyKind::Surface: {
        QStringList items;
        for (const auto& c : context.surfaces) items << qstr(c.name);
        w = combo(p.name, items, current, qstr(p.doc));
        break;
      }
      case proc::PropertyKind::BoundaryFace: {
        // V1: the boundary's named faces from the last evaluation; one it
        // names that isn't listed (yet) stays as it is.
        QStringList items{whole_boundary_text()};
        for (const auto& f : context.boundary_faces) items << qstr(f.label);
        const QString shown = current.isEmpty() ? whole_boundary_text() : current;
        if (!items.contains(shown)) items << shown;
        w = combo(p.name, items, shown, qstr(p.doc));
        break;
      }
      case proc::PropertyKind::Parameter: {
        QStringList items;
        for (const auto& gp : graph.parameters) items << qstr(gp.name);
        w = combo(p.name, items, current, qstr(p.doc));
        break;
      }
      case proc::PropertyKind::Template: {
        // L4: the composite's templates; empty makes placeholders.
        QStringList items{QString()};
        for (const auto& t : context.templates) items << qstr(t);
        w = combo(p.name, items, current, qstr(p.doc));
        break;
      }
      default: w = line(p.name, current, qstr(p.doc)); break;
    }
    form_->addRow(qstr(p.name), w);
  }
}

// --- the template library ------------------------------------------------------------------------

TemplateDialog::TemplateDialog(const model::Document& document, const Id& volume, QWidget* parent)
    : EditDialog(tr("Attach a template"), parent), context_(graph_context(document, volume)) {
  list_ = new QListWidget;
  for (const auto& t : proc::templates()) {
    auto* item = new QListWidgetItem(qstr(t.name), list_);
    item->setToolTip(qstr(t.doc));
  }
  list_->setMinimumHeight(110);
  form()->addRow(tr("Template"), list_);
  doc_ = new QLabel;
  doc_->setWordWrap(true);
  form()->addRow(QString(), doc_);
  cell_ = new QLineEdit(QStringLiteral("0.5"));
  cell_->setToolTip(tr("The output's grid in mm: generation cost grows with the inverse cube"));
  form()->addRow(tr("Cell size (mm)"), cell_);
  surface_ = new QComboBox;
  // Bolted flanges first: they're what bolt_bosses reads.
  std::vector<GraphContext::Surface> surfaces = context_.surfaces;
  std::stable_sort(surfaces.begin(), surfaces.end(), [](const auto& a, const auto& b) {
    return (a.type == model::ConnectionType::BoltedFlange) > (b.type == model::ConnectionType::BoltedFlange);
  });
  context_.surfaces = surfaces;
  for (const auto& s : context_.surfaces) surface_->addItem(qstr(s.name));
  form()->addRow(tr("Reads surface"), surface_);
  connect(list_, &QListWidget::currentRowChanged, this, [this] {
    update_doc();
    emit changed();
  });
  watch(cell_);
  watch(surface_);
  if (list_->count() > 0) list_->setCurrentRow(0);
  update_doc();
}

void TemplateDialog::update_doc() {
  const int row = list_->currentRow();
  if (row < 0 || static_cast<std::size_t>(row) >= proc::templates().size()) {
    doc_->clear();
    surface_->setEnabled(false);
    return;
  }
  const auto& t = proc::templates()[static_cast<std::size_t>(row)];
  doc_->setText(qstr(t.doc));
  surface_->setEnabled(t.needs_surface);
  if (t.needs_surface && context_.surfaces.empty()) {
    set_message(tr("%1 reads a connection surface, and none joins this volume.").arg(qstr(t.name)));
  } else {
    set_message({});
  }
}

std::string TemplateDialog::template_name() const {
  const int row = list_->currentRow();
  if (row < 0 || static_cast<std::size_t>(row) >= proc::templates().size()) return {};
  return proc::templates()[static_cast<std::size_t>(row)].name;
}

void TemplateDialog::choose(const std::string& name) {
  for (std::size_t i = 0; i < proc::templates().size(); ++i) {
    if (proc::templates()[i].name == name) list_->setCurrentRow(static_cast<int>(i));
  }
}

void TemplateDialog::set_cell_size(double mm) { cell_->setText(num(mm)); }

bool TemplateDialog::complete() const { return graph().has_value(); }

std::optional<proc::Graph> TemplateDialog::graph() const {
  const std::string name = template_name();
  if (name.empty()) return std::nullopt;
  bool ok = false;
  const double cell = cell_->text().trimmed().toDouble(&ok);
  if (!ok || !(cell > 0.0)) return std::nullopt;
  proc::TemplateOptions options;
  options.cell_size = cell;
  const int s = surface_->currentIndex();
  if (s >= 0 && static_cast<std::size_t>(s) < context_.surfaces.size()) {
    options.surface = context_.surfaces[static_cast<std::size_t>(s)].id;
  }
  auto g = proc::make_template(name, options);
  if (!g) return std::nullopt;
  return std::move(g).value();
}

}  // namespace vs::app

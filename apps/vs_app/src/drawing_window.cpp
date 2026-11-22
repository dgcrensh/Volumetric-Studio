#include "drawing_window.hpp"

#include <QFontMetricsF>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>

#include "widgets.hpp"

namespace vs::app {

namespace {

struct PenLook {
  double width;  // sheet mm
  QColor colour;
  std::vector<double> dash;  // sheet mm, empty: solid
};

// The same weights, dashes and colours as vs_draw's writers.
const PenLook& look(draw::Pen pen) {
  static const PenLook kBorder{0.5, QColor(0, 0, 0), {}};
  static const PenLook kVisible{0.35, QColor(0, 0, 0), {}};
  static const PenLook kHidden{0.25, QColor(0, 0, 0), {3.0, 1.5}};
  static const PenLook kBoundary{0.18, QColor(0x4a, 0x6f, 0xa5), {8.0, 1.5, 1.5, 1.5, 1.5, 1.5}};
  static const PenLook kSurface{0.25, QColor(0x1b, 0x8a, 0x7a), {6.0, 1.5, 1.0, 1.5}};
  static const PenLook kDimension{0.18, QColor(0, 0, 0), {}};
  static const PenLook kDisposable{0.18, QColor(0xb3, 0x5c, 0x00), {}};
  switch (pen) {
    case draw::Pen::Border: return kBorder;
    case draw::Pen::Visible: return kVisible;
    case draw::Pen::Hidden: return kHidden;
    case draw::Pen::Boundary: return kBoundary;
    case draw::Pen::Surface: return kSurface;
    case draw::Pen::Dimension: return kDimension;
    case draw::Pen::Disposable: return kDisposable;
  }
  return kVisible;
}

// A pen in sheet units for a painter scaled by `k` pixels per sheet mm; at
// least a pixel wide, so thin lines never vanish on a small view.
QPen qpen(draw::Pen pen, double k) {
  const PenLook& l = look(pen);
  QPen p(l.colour);
  p.setCapStyle(Qt::RoundCap);
  p.setJoinStyle(Qt::RoundJoin);
  double unit = l.width;  // what dash lengths are measured in
  if (l.width * k < 1.0) {
    p.setCosmetic(true);
    p.setWidthF(1.0);
    unit = 1.0 / k;
  } else {
    p.setWidthF(l.width);
  }
  if (!l.dash.empty()) {
    QList<qreal> pattern;
    for (const double d : l.dash) pattern.append(std::max(d / unit, 0.5));
    p.setDashPattern(pattern);
    p.setCapStyle(Qt::FlatCap);
  }
  return p;
}

}  // namespace

QRectF paint_sheet(QPainter& painter, const draw::Sheet& sheet, const QRectF& target) {
  if (sheet.width <= 0 || sheet.height <= 0 || target.width() <= 0 || target.height() <= 0) return {};
  const double k = std::min(target.width() / sheet.width, target.height() / sheet.height);
  const QRectF paper(target.center().x() - sheet.width * k / 2, target.center().y() - sheet.height * k / 2,
                     sheet.width * k, sheet.height * k);
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::TextAntialiasing);
  painter.fillRect(paper, Qt::white);

  // Sheet millimetres, y up from the lower left corner.
  QTransform to_sheet;
  to_sheet.translate(paper.left(), paper.bottom());
  to_sheet.scale(k, -k);

  painter.setTransform(to_sheet, true);
  for (const auto& path : sheet.paths) {
    if (path.points.size() < 2) continue;
    QPolygonF poly;
    poly.reserve(static_cast<qsizetype>(path.points.size()));
    for (const auto& p : path.points) poly.append(QPointF(p.x, p.y));
    if (path.filled) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(look(path.pen).colour);
      painter.drawPolygon(poly);
      continue;
    }
    painter.setPen(qpen(path.pen, k));
    painter.setBrush(Qt::NoBrush);
    if (path.closed) {
      painter.drawPolygon(poly);
    } else {
      painter.drawPolyline(poly);
    }
  }
  painter.restore();

  // Texts in device space (fonts don't take a mirrored transform): a font
  // of 100 px scaled down to the text's height in sheet mm.
  QFont font(QStringLiteral("Arial"));
  font.setPixelSize(100);
  const QFontMetricsF metrics(font);
  painter.save();
  painter.setRenderHint(QPainter::TextAntialiasing);
  painter.setFont(font);
  for (const auto& t : sheet.texts) {
    if (t.text.empty() || t.height <= 0) continue;
    QString text = qstr(t.text);
    text.replace(QChar(0x2300), QChar(0xD8));  // the diameter sign, as Arial has it
    const double w = metrics.horizontalAdvance(text);
    const double dx = t.align == draw::TextAlign::Left ? 0.0 : t.align == draw::TextAlign::Centre ? -w / 2 : -w;
    const QPointF at(paper.left() + t.at.x * k, paper.bottom() - t.at.y * k);
    painter.save();
    painter.translate(at);
    if (t.vertical) painter.rotate(-90.0);
    const double s = t.height * k / 100.0;
    painter.scale(s, s);
    painter.setPen(look(t.pen).colour);
    painter.drawText(QPointF(dx, 0.0), text);
    painter.restore();
  }
  painter.restore();
  return paper;
}

// --- SheetView ------------------------------------------------------------------------------

SheetView::SheetView(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("sheet_view"));
  setAutoFillBackground(false);
  setMinimumSize(320, 240);
}

void SheetView::set_sheet(draw::Sheet sheet) {
  sheet_ = std::move(sheet);
  has_sheet_ = true;
  update();
}

void SheetView::clear() {
  sheet_ = {};
  has_sheet_ = false;
  update();
}

namespace {

void paint_view(QPainter& p, const QRectF& area, const draw::Sheet* sheet) {
  p.fillRect(area, QColor(0x6b, 0x72, 0x7c));
  if (sheet == nullptr) {
    p.setPen(QColor(0xe8, 0xea, 0xee));
    p.drawText(area, Qt::AlignCenter, QObject::tr("Drawing…"));
    return;
  }
  const QRectF target = area.adjusted(16, 16, -16, -16);
  const QRectF paper = paint_sheet(p, *sheet, target);
  // A drop shadow's edge, so the paper reads as a sheet.
  p.setPen(QPen(QColor(0x40, 0x44, 0x4a), 1));
  p.setBrush(Qt::NoBrush);
  p.drawRect(paper);
}

}  // namespace

void SheetView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  paint_view(p, QRectF(rect()), has_sheet_ ? &sheet_ : nullptr);
}

QImage SheetView::render_image(const QSize& size) const {
  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter p(&image);
  paint_view(p, QRectF(QPointF(0, 0), QSizeF(size)), has_sheet_ ? &sheet_ : nullptr);
  p.end();
  return image;
}

// --- DrawingWindow -----------------------------------------------------------------------

DrawingWindow::DrawingWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
  setObjectName(QStringLiteral("drawing_window"));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto* bar = new QToolBar;
  bar->addAction(tr("Export…"), this, &DrawingWindow::exportRequested);
  layout->addWidget(bar);
  view_ = new SheetView;
  layout->addWidget(view_, 1);
  status_ = new QLabel;
  status_->setContentsMargins(8, 3, 8, 3);
  status_->setWordWrap(true);
  layout->addWidget(status_);
  set_title({});
  resize(1100, 820);
}

void DrawingWindow::set_title(const QString& name) {
  setWindowTitle(name.isEmpty() ? tr("Drawing") : tr("Drawing — %1").arg(name));
}

void DrawingWindow::set_drawing(const core::Id& drawing, const QString& name) {
  if (drawing != drawing_) {
    drawing_ = drawing;
    result_.reset();
    view_->clear();
    status_->clear();
  }
  set_title(name);
}

void DrawingWindow::show_result(std::shared_ptr<const draw::DrawingResult> result) {
  result_ = std::move(result);
  if (!result_) {
    view_->clear();
    status_->clear();
    return;
  }
  view_->set_sheet(result_->sheet);
  std::size_t errors = 0;
  std::size_t warnings = 0;
  QString first;
  for (const auto& d : result_->diagnostics) {
    if (d.severity == core::Severity::Error) ++errors;
    if (d.severity == core::Severity::Warning) ++warnings;
    if (first.isEmpty() && d.severity != core::Severity::Info) first = qstr(d.message);
  }
  std::size_t lost = 0;
  for (const auto& d : result_->dimensions) {
    if (!d.drawn) ++lost;
  }
  if (errors == 0 && warnings == 0) {
    status_->setText(tr("%1 views, %2 dimensions").arg(result_->views.size()).arg(result_->dimensions.size() - lost));
    status_->setStyleSheet({});
  } else {
    status_->setText(tr("%1 errors, %2 warnings (%3 dimensions not drawn): %4")
                         .arg(errors)
                         .arg(warnings)
                         .arg(lost)
                         .arg(first));
    status_->setStyleSheet(errors > 0 ? QStringLiteral("QLabel { color: #b02020; }")
                                      : QStringLiteral("QLabel { color: #8a5a00; }"));
  }
}

QString DrawingWindow::status_text() const { return status_->text(); }

}  // namespace vs::app

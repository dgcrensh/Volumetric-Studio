#include "icon_painter.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRectF>

#include <algorithm>
#include <utility>

namespace vs::app {

namespace {

constexpr double kPenWidth = 1.75;  // grid units
constexpr double kDotRadius = 1.6;  // grid units

}  // namespace

void paint_icon(QPainter& painter, Icon icon, const QRectF& rect, const QColor& color) {
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  painter.translate(rect.topLeft());
  const double scale = std::min(rect.width(), rect.height()) / kIconGrid;
  painter.scale(scale, scale);
  QPen pen(color, kPenWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  for (const IconStroke& s : icon_strokes(icon)) {
    if (s.points.empty()) continue;
    if (s.dot) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(color);
      painter.drawEllipse(QPointF(s.points.front().x, s.points.front().y), kDotRadius, kDotRadius);
      continue;
    }
    QPen p = pen;
    if (s.dashed) {
      p.setCapStyle(Qt::FlatCap);
      p.setDashPattern({1.6, 1.4});  // in pen widths
    }
    painter.setPen(p);
    painter.setBrush(Qt::NoBrush);
    QPainterPath path(QPointF(s.points.front().x, s.points.front().y));
    for (std::size_t i = 1; i < s.points.size(); ++i) path.lineTo(s.points[i].x, s.points[i].y);
    if (s.closed) path.closeSubpath();
    painter.drawPath(path);
  }
  painter.restore();
}

QIcon tool_icon(Icon icon, const QColor& color, const QColor& disabled, int size) {
  QIcon out;
  for (const int scale : {1, 2}) {
    for (const auto& [mode, c] : {std::pair{QIcon::Normal, color}, std::pair{QIcon::Disabled, disabled}}) {
      QPixmap pm(size * scale, size * scale);
      pm.fill(Qt::transparent);
      {
        QPainter p(&pm);
        paint_icon(p, icon, QRectF(0, 0, size * scale, size * scale), c);
      }
      pm.setDevicePixelRatio(scale);
      out.addPixmap(pm, mode);
    }
  }
  return out;
}

}  // namespace vs::app

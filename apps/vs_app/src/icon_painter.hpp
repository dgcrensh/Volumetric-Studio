#pragma once

// Paints the catalogue's icons (icons.hpp) with Qt (U1, decision 0039):
// each stroke with a round pen 1.75 grid units wide, dots filled, dashed
// strokes dashed, in one colour, so an icon follows the theme's text colour
// and stays crisp at any size.

#include <QColor>
#include <QIcon>

#include "icons.hpp"

class QPainter;
class QRectF;

namespace vs::app {

// Paints `icon` into `rect` (a square; the 24-unit grid is scaled to it).
void paint_icon(QPainter& painter, Icon icon, const QRectF& rect, const QColor& color);

// The icon as a QIcon of `size` pixels (also at twice that, for high-DPI
// screens): `color` when enabled, `disabled` when not.
QIcon tool_icon(Icon icon, const QColor& color, const QColor& disabled, int size = 24);

}  // namespace vs::app

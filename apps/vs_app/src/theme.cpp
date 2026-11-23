#include "theme.hpp"

#include <QPalette>
#include <QWidget>

namespace vs::app::theme {

namespace {

QString hex(const QColor& c) { return c.name(QColor::HexRgb); }

}  // namespace

QString style_sheet() {
  return QStringLiteral(
             "QMainWindow { background: %1; }"
             "QMainWindow::separator { background: %2; width: 1px; height: 1px; }"
             "QToolBar#tools { background: %3; border: none; border-bottom: 1px solid %2; }"
             "QStatusBar { background: %4; border-top: 1px solid %2; color: %5; }"
             "QStatusBar::item { border: none; }"
             "QDockWidget { color: %5; }"
             "QDockWidget::title { background: %4; padding: 4px 6px; border-bottom: 1px solid %2; }"
             "#browser, #inspector_panel, #problems_drawer { background: %3; }"
             "#problems_drawer { border-top: 1px solid %6; }"
             "QSplitter::handle { background: %2; }"
             "QSplitter::handle:vertical { height: 4px; }"
             "QTreeView, QTableView { background: %3; alternate-background-color: %4; border: none; "
             "selection-background-color: %7; selection-color: %8; }"
             "QHeaderView::section { background: %4; color: %5; border: none; "
             "border-bottom: 1px solid %2; padding: 3px 6px; }"
             "QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border: 1px solid %9; }")
      .arg(hex(kWindow), hex(kBorder), hex(kPanel), hex(kPanelAlt), hex(kTextMuted), hex(kBorderStrong),
           hex(kSelection), hex(kText), hex(kAccent));
}

void apply(QWidget& window) {
  QPalette p = window.palette();
  for (QPalette::ColorGroup g : {QPalette::Active, QPalette::Inactive}) {
    p.setColor(g, QPalette::Window, kWindow);
    p.setColor(g, QPalette::WindowText, kText);
    p.setColor(g, QPalette::Base, kPanel);
    p.setColor(g, QPalette::AlternateBase, kPanelAlt);
    p.setColor(g, QPalette::Text, kText);
    p.setColor(g, QPalette::Button, kPanel);
    p.setColor(g, QPalette::ButtonText, kText);
    p.setColor(g, QPalette::Highlight, kAccent);
    p.setColor(g, QPalette::HighlightedText, Qt::white);
    p.setColor(g, QPalette::Mid, kBorderStrong);
    p.setColor(g, QPalette::Dark, kTextFaint);
    p.setColor(g, QPalette::PlaceholderText, kTextFaint);
  }
  p.setColor(QPalette::Disabled, QPalette::WindowText, kTextFaint.lighter(130));
  p.setColor(QPalette::Disabled, QPalette::ButtonText, kTextFaint.lighter(130));
  p.setColor(QPalette::Disabled, QPalette::Text, kTextFaint.lighter(130));
  window.setPalette(p);
  window.setStyleSheet(style_sheet());
}

}  // namespace vs::app::theme

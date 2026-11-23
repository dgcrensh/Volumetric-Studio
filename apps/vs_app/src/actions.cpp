#include "actions.hpp"

#include <QAction>
#include <QApplication>
#include <QKeySequence>
#include <QPalette>
#include <QWidget>

#include "icon_painter.hpp"

namespace vs::app {

namespace {

QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

}  // namespace

QString tool_tip(const Tool& tool, const Availability& availability) {
  QString tip = qs(tool.tooltip);
  if (!tool.shortcut.empty()) {
    tip += QStringLiteral(" (%1)").arg(QKeySequence(qs(tool.shortcut)).toString(QKeySequence::NativeText));
  }
  if (!availability.enabled && !availability.reason.empty()) tip += QStringLiteral("\n") + qs(availability.reason);
  return tip;
}

ToolActions::ToolActions(QWidget* owner) : QObject(owner) {
  const QPalette palette = owner->palette();
  const QColor color = palette.color(QPalette::Active, QPalette::ButtonText);
  const QColor disabled = palette.color(QPalette::Disabled, QPalette::ButtonText);
  for (const Tool& t : all_tools()) {
    auto* a = new QAction(qs(t.label), owner);
    a->setObjectName(QStringLiteral("tool_") + qs(t.name));
    a->setIconText(qs(t.label));
    a->setIcon(tool_icon(t.icon, color, disabled));
    a->setToolTip(tool_tip(t, {}));
    a->setStatusTip(qs(t.tooltip));
    if (!t.shortcut.empty()) a->setShortcut(QKeySequence(qs(t.shortcut)));
    owner->addAction(a);
    const ToolId id = t.id;
    connect(a, &QAction::triggered, this, [this, id] { emit triggered(id); });
    actions_.at(static_cast<std::size_t>(id)) = a;
  }
}

QAction* ToolActions::action(ToolId id) const { return actions_.at(static_cast<std::size_t>(id)); }

void ToolActions::refresh(const model::Document& document, const ToolState& state) {
  for (const Tool& t : all_tools()) {
    const Availability a = availability(t.id, document, state);
    QAction* action = actions_.at(static_cast<std::size_t>(t.id));
    action->setEnabled(a.enabled);
    action->setToolTip(tool_tip(t, a));
  }
}

}  // namespace vs::app

#pragma once

// One QAction per catalogue tool (U1, decision 0039). The context bar's
// buttons, its "More" menus and (from later phases) the right-click menus
// all show these same actions, so a shortcut has exactly one holder and
// tests find an action by its text or its object name ("tool_<name>").
//
// The actions are added to the owner (the main window), so their
// shortcuts work whichever toolbar is showing; refresh() enables each one
// by availability() and puts the reason in its tooltip when it can't act.
// Triggering one emits triggered(id); the window runs the tool.

#include <QObject>

#include <array>

#include "tool_catalog.hpp"

class QAction;
class QWidget;

namespace vs::app {

class ToolActions : public QObject {
  Q_OBJECT

 public:
  explicit ToolActions(QWidget* owner);

  [[nodiscard]] QAction* action(ToolId id) const;
  // Enables every action by availability(); a disabled one's tooltip says why.
  void refresh(const model::Document& document, const ToolState& state);

 signals:
  void triggered(vs::app::ToolId id);

 private:
  std::array<QAction*, static_cast<std::size_t>(ToolId::CloseDrawing) + 1> actions_{};
};

// "Extrude a sketch's profile (Ctrl+Shift+E)", then the reason on its own
// line when there is one.
QString tool_tip(const Tool& tool, const Availability& availability);

}  // namespace vs::app

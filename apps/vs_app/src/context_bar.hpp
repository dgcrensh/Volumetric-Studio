#pragma once

// The context bar (U1, decision 0039): a row of tabs (Layout, Contents,
// Sketch, Graph, Drawing) above one toolbar, the current context's.
//
//   [Layout] [Contents] [Sketch] [Graph] [Drawing]  detail     corner widgets
//   +-------------------------------------------------------------------------+
//   | Volume Surface Keep-out Link | Sketch Plane Point Extrude More | ...    |
//   |            Create            |        Layout geometry          |        |
//   +-------------------------------------------------------------------------+
//
// Each toolbar is the context's groups (tool_groups()): the group's first
// `shown` tools as buttons with their label under the icon, the rest in the
// group's "More" menu, and the group's name under them. Every button shows
// one of ToolActions' actions, so enabling, tooltips and shortcuts follow
// the actions.
//
// The bar never changes context by itself: clicking a tab emits
// tabRequested(), the window enters that context (or says why it can't)
// and calls set_context(). Tabs that can't be entered are dimmed, with the
// reason as their tooltip.

#include <QWidget>

#include <array>

#include "tool_catalog.hpp"

class QHBoxLayout;
class QLabel;
class QStackedWidget;
class QToolButton;

namespace vs::app {

class ToolActions;

class ContextBar : public QWidget {
  Q_OBJECT

 public:
  explicit ContextBar(const ToolActions& actions, QWidget* parent = nullptr);

  void set_context(Context context);
  [[nodiscard]] Context context() const { return context_; }
  // Enables a tab, or dims it with the reason as its tooltip.
  void set_tab(Context tab, const Availability& availability);
  // The text beside the tabs: what the context is working on.
  void set_detail(const QString& text);
  [[nodiscard]] QString detail() const;
  // Widgets on the right of the tab row (the configuration box, Regenerate).
  void add_corner_widget(QWidget* widget);

  [[nodiscard]] QToolButton* tab(Context context) const;
  // A context's toolbar.
  [[nodiscard]] QWidget* page(Context context) const;
  // The button showing `id` on `context`'s toolbar; null when the tool is
  // in a "More" menu there, or not on that toolbar.
  [[nodiscard]] QToolButton* button(Context context, ToolId id) const;
  // The "More" button of `context`'s group `group` (null: the group has none).
  [[nodiscard]] QToolButton* more_button(Context context, std::size_t group) const;

 signals:
  void tabRequested(vs::app::Context tab);

 private:
  QWidget* make_page(Context context, const ToolActions& actions);

  Context context_ = Context::Layout;
  std::array<QToolButton*, std::size(kContexts)> tabs_{};
  QLabel* detail_ = nullptr;
  QHBoxLayout* corner_ = nullptr;
  QStackedWidget* pages_ = nullptr;
};

}  // namespace vs::app

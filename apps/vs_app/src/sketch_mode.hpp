#pragma once

// Sketch mode (plan, M14): a 2D canvas on the sketch's plane, drawing tools,
// constraint and dimension buttons, the constraint list with editable
// dimensions, and the solver's status (degrees of freedom; conflicts by
// name), live after every change. The rules are SketchEditor's
// (sketch_editor.hpp); these widgets only draw it and turn input into
// calls.
//
// SketchView, the canvas:
//   - the left button draws with the current tool, snapping to points
//     within 8 px; with the select tool it picks the nearest entity (Ctrl
//     adds to the selection) and drags points (drag solving), one drag one
//     undo step;
//   - the right button or Esc ends a chain of lines; the middle button
//     pans and the wheel zooms about the cursor; Delete removes the
//     selection;
//   - geometry is drawn by state: selected orange, part of a conflict red,
//     still free blue, fully fixed dark green, construction dashed grey;
//     dimensions show their value (and expression).
//
// SketchMode, the panel around it, ends with finished(true) (Finish) or
// finished(false) (Cancel); the main window commits the sketch on Finish.

#include <QWidget>

#include <memory>
#include <vector>

#include "sketch_editor.hpp"

class QAction;
class QLabel;
class QToolBar;
class QTreeWidget;

namespace vs::app {

class SketchView : public QWidget {
  Q_OBJECT

 public:
  explicit SketchView(SketchEditor& editor, QWidget* parent = nullptr);

  [[nodiscard]] const std::vector<Id>& selection() const { return selection_; }
  void set_selection(std::vector<Id> ids);
  // Frames the sketch (or a 100 mm square round the origin when empty).
  void fit();
  // Screen position of a sketch point, and back.
  [[nodiscard]] QPointF to_screen(dvec2 p) const;
  [[nodiscard]] dvec2 to_sketch(QPointF p) const;

 signals:
  void selectionChanged();
  // The sketch changed (a shape, a drag, a deletion).
  void edited();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  [[nodiscard]] double pixel() const;  // one pixel in sketch units

  SketchEditor& editor_;
  std::vector<Id> selection_;
  double scale_ = 4.0;  // pixels per mm
  QPointF origin_;      // screen position of the sketch origin
  bool fitted_ = false;
  QPointF hover_;
  QPoint pan_from_;
  bool panning_ = false;
  Id dragging_;
  bool drag_started_ = false;
};

class SketchMode : public QWidget {
  Q_OBJECT

 public:
  // `title` names the sketch ("Base outline on XY plane").
  SketchMode(std::unique_ptr<SketchEditor> editor, const QString& title, QWidget* parent = nullptr);
  ~SketchMode() override;

  [[nodiscard]] SketchEditor& editor() { return *editor_; }
  [[nodiscard]] SketchView& view() { return *view_; }
  [[nodiscard]] QTreeWidget& constraint_list() { return *constraints_; }
  [[nodiscard]] QString status_text() const;

  // The buttons' actions, callable directly.
  void set_tool(SketchTool tool);
  // Adds a constraint of `kind` on the view's selection (dimensions take
  // their measured value); false when it doesn't fit.
  bool constrain(sketch::ConstraintKind kind);
  // Edits row `row`'s dimension from text ("25", "25 mm", "width / 2").
  bool set_dimension_text(const Id& constraint, const QString& text);
  void undo();
  void redo();
  void remove_selection();
  void toggle_construction();
  void finish(bool accept);

  // Refreshes the list, the status and the canvas from the editor.
  void refresh();

 signals:
  void finished(bool accept);

 private:
  void update_actions();

  std::unique_ptr<SketchEditor> editor_;
  SketchView* view_ = nullptr;
  QTreeWidget* constraints_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* hint_ = nullptr;
  std::vector<std::pair<sketch::ConstraintKind, QAction*>> constraint_actions_;
  std::vector<std::pair<SketchTool, QAction*>> tool_actions_;
  QAction* undo_ = nullptr;
  QAction* redo_ = nullptr;
  bool refreshing_ = false;
};

}  // namespace vs::app

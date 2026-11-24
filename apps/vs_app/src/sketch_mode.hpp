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
//     dimensions show their value (and expression);
//   - dragging a dimension's label (M16) changes its value along the
//     dimension's direction, solving as it goes (one drag one undo step);
//     a dimension an expression drives says so instead.
//
// SketchMode, the canvas and the sketch's side panel, ends with
// finished(true) (Finish) or finished(false) (Cancel); the main window
// commits the sketch on Finish. Since U1 (decision 0039) the tools, the
// constraints and Finish and Cancel are the window's Sketch tab, which
// calls set_tool(), constrain(), toggle_construction(), remove_selection()
// and finish(); the side panel (the solver's status and the constraint
// list with editable dimensions) is shown by the window in its right-hand
// dock while sketching. SketchMode deletes the panel with itself.

#include <QPointer>
#include <QWidget>

#include <memory>
#include <vector>

#include "sketch_editor.hpp"

class QAction;
class QLabel;
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
  // Where a dimension's label was last drawn (empty if it wasn't).
  [[nodiscard]] QRectF dimension_label(const Id& constraint) const;

 signals:
  void selectionChanged();
  // The sketch changed (a shape, a drag, a deletion).
  void edited();
  // Something to tell the user (a dimension that can't be dragged).
  void message(const QString& text);

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
  // M16: dimension labels as last drawn, and a dimension being dragged.
  mutable std::vector<std::pair<Id, QRectF>> labels_;
  Id dragging_dimension_;
  double dimension_start_ = 0.0;
  QPointF dimension_from_;
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
  // The status, constraint list and hint, for the window's right-hand dock.
  [[nodiscard]] QWidget* side_panel() const { return side_; }
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

  // S1: Import SVG previews on the sketch. Each preview replaces the last;
  // keep_import() keeps it as one undo step, drop_import() takes it out.
  core::Result<sketch::SvgImport> preview_import(std::string_view svg, const sketch::SvgImportOptions& options);
  void keep_import();
  void drop_import();
  [[nodiscard]] bool import_previewing() const { return import_previewing_; }

 signals:
  void finished(bool accept);
  // The drawing tool changed (set_tool()).
  void toolChanged();

 private:
  void update_actions();

  std::unique_ptr<SketchEditor> editor_;
  SketchView* view_ = nullptr;
  QPointer<QWidget> side_;  // the side panel, wherever the window put it
  QTreeWidget* constraints_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* hint_ = nullptr;
  QAction* undo_ = nullptr;
  QAction* redo_ = nullptr;
  bool refreshing_ = false;
  bool import_previewing_ = false;
};

}  // namespace vs::app

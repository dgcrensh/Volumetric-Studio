#pragma once

// The drawing window (L6, decision 0038): one drawing's sheet, drawn with
// QPainter from vs_draw's Sheet.
//
// SheetView paints a draw::Sheet on white paper, fitted to the widget with
// a margin, y flipped (a sheet's origin is its lower left corner, y up).
// Each pen has its own look, as in the files vs_draw writes: the border and
// visible edges solid (visible ones thicker), hidden edges dashed and thin,
// boundaries a long-dash phantom line, connection surfaces dash-dot,
// dimensions thin, and disposable dimensions (on generated contents) in a
// colour of their own. Texts keep their alignment and quarter turns.
//
// DrawingWindow is one page of the main window's central stack (U1 phase
// 6: the Drawing tab shows it in place of the viewport), holding a
// SheetView and a line saying what draw() reported; the Drawing toolbar
// exports it. The main window computes the sheet (draw::draw() on the
// session's worker, after each evaluation and each edit of the drawing)
// and hands it over
// with show_result(); nothing here evaluates or edits anything.

#include <QWidget>

#include <memory>

#include "vs/core/id.hpp"
#include "vs/draw/drawing.hpp"
#include "vs/draw/sheet.hpp"

class QLabel;
class QPainter;
class QToolBar;

namespace vs::app {

// Paints `sheet` into `target` (device pixels), keeping its aspect ratio and
// centring it there; white paper, black lines. Returns the sheet's rectangle.
QRectF paint_sheet(QPainter& painter, const draw::Sheet& sheet, const QRectF& target);

class SheetView : public QWidget {
  Q_OBJECT

 public:
  explicit SheetView(QWidget* parent = nullptr);

  void set_sheet(draw::Sheet sheet);
  void clear();
  [[nodiscard]] const draw::Sheet& sheet() const { return sheet_; }
  [[nodiscard]] bool has_sheet() const { return has_sheet_; }
  // The sheet as the view paints it, rendered off screen at `size`.
  [[nodiscard]] QImage render_image(const QSize& size) const;

  [[nodiscard]] QSize sizeHint() const override { return {800, 600}; }

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  draw::Sheet sheet_;
  bool has_sheet_ = false;
};

class DrawingWindow : public QWidget {
  Q_OBJECT

 public:
  explicit DrawingWindow(QWidget* parent = nullptr);

  // The drawing shown (null: none); set_drawing() clears the sheet until
  // its first result arrives.
  void set_drawing(const core::Id& drawing, const QString& name);
  [[nodiscard]] const core::Id& drawing() const { return drawing_; }
  void set_title(const QString& name);

  // A result of draw() for the drawing shown.
  void show_result(std::shared_ptr<const draw::DrawingResult> result);
  [[nodiscard]] std::shared_ptr<const draw::DrawingResult> result() const { return result_; }

  [[nodiscard]] SheetView& view() { return *view_; }
  [[nodiscard]] QString status_text() const;

 signals:
  // Export asked for (the main window's Drawing toolbar exports too).
  void exportRequested();

 private:
  core::Id drawing_;
  SheetView* view_ = nullptr;
  QLabel* status_ = nullptr;
  std::shared_ptr<const draw::DrawingResult> result_;
};

}  // namespace vs::app

#pragma once

// The Problems drawer and its status-bar count (U1 phase 5, decision 0039).
//
// ProblemsButton sits at the right of the status bar: an error icon and a
// count, a warning icon and a count, or "No problems". Clicking it opens
// or closes the drawer.
//
// ProblemsDrawer is a pane under the viewport, in a vertical splitter with
// it, so opening it pushes the viewport up instead of covering it (the
// viewport is a native window: nothing can sit on top of it). It holds the
// window's diagnostics list (MainWindow::diagnostics_list()) under a
// header naming where the problems came from. The window opens it only
// after something the person started fails (Validate, a refused edit, an
// export check, an import, a bake); a background evaluation only updates
// the count.

#include <QString>
#include <QToolButton>
#include <QWidget>

#include <cstddef>

class QLabel;

namespace vs::app {

class ProblemsButton : public QToolButton {
  Q_OBJECT

 public:
  explicit ProblemsButton(QWidget* parent = nullptr);
  void set_counts(std::size_t errors, std::size_t warnings);
  [[nodiscard]] std::size_t errors() const { return errors_; }
  [[nodiscard]] std::size_t warnings() const { return warnings_; }

 private:
  std::size_t errors_ = 0;
  std::size_t warnings_ = 0;
};

class ProblemsDrawer : public QWidget {
  Q_OBJECT

 public:
  // `list` is the diagnostics list the drawer shows; it becomes the
  // drawer's child.
  explicit ProblemsDrawer(QWidget* list, QWidget* parent = nullptr);
  // "Validation: 2 errors, 1 warning".
  void set_summary(const QString& source, std::size_t errors, std::size_t warnings);
  [[nodiscard]] QString summary() const;
  [[nodiscard]] QToolButton& close_button() const { return *close_; }

 signals:
  void closeRequested();

 private:
  QLabel* summary_ = nullptr;
  QToolButton* close_ = nullptr;
};

}  // namespace vs::app

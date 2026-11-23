#pragma once

// The Browser and the panels that fold to a strip (U1 phase 4, decision
// 0039).
//
// FoldPanel is a side panel with a header (its title and a fold button)
// above its body. Folded, it is a strip a few pixels wider than an icon:
// the unfold button and one button per section, so the viewport gets the
// room. The main window resizes the dock holding it when it folds or
// unfolds (foldedChanged()).
//
// Browser is the left panel: one FoldPanel whose body is a column of
// sections (Volumes, Features, Variables, Drawings), each with a header
// that folds it to a line; open sections share the height. Clicking a
// section's strip button unfolds the Browser with that section open. The
// sections hold the window's existing trees, so volume_tree(),
// feature_tree(), variables_table() and drawings_tree() keep working.

#include <QString>
#include <QWidget>

#include <vector>

#include "icons.hpp"

class QLabel;
class QToolButton;
class QVBoxLayout;

namespace vs::app {

class FoldPanel : public QWidget {
  Q_OBJECT

 public:
  // `side`: where the panel sits (Qt::LeftEdge or Qt::RightEdge), so the
  // fold button points the way the panel folds.
  FoldPanel(const QString& title, Qt::Edge side, QWidget* parent = nullptr);

  void set_body(QWidget* body);
  [[nodiscard]] QWidget* body() const { return body_; }
  // A button on the folded strip; clicking it unfolds the panel and emits
  // stripClicked(index).
  int add_strip_button(Icon icon, const QString& tooltip);

  void set_folded(bool folded);
  [[nodiscard]] bool folded() const { return folded_; }
  // The fold button in the header, and the unfold button on the strip.
  [[nodiscard]] QToolButton& fold_button() const { return *fold_; }
  [[nodiscard]] QToolButton& unfold_button() const { return *unfold_; }
  // The folded strip's width.
  [[nodiscard]] static int strip_width();

 signals:
  void foldedChanged(bool folded);
  void stripClicked(int index);

 private:
  bool folded_ = false;
  QWidget* expanded_ = nullptr;  // header and body
  QWidget* strip_ = nullptr;
  QVBoxLayout* strip_layout_ = nullptr;
  QVBoxLayout* expanded_layout_ = nullptr;
  QWidget* body_ = nullptr;
  QToolButton* fold_ = nullptr;
  QToolButton* unfold_ = nullptr;
  int strip_buttons_ = 0;
};

class Browser : public FoldPanel {
  Q_OBJECT

 public:
  explicit Browser(QWidget* parent = nullptr);

  // Adds a section holding `content`, open unless `open` is false.
  int add_section(const QString& title, Icon icon, QWidget* content, bool open = true);
  [[nodiscard]] int section_count() const { return static_cast<int>(sections_.size()); }
  void set_section_open(int section, bool open);
  [[nodiscard]] bool section_open(int section) const;
  [[nodiscard]] QToolButton& section_header(int section) const;
  // A count after a section's title (U1 phase 5: how many of its rows have
  // problems); empty clears it. `tooltip` says what it counts.
  void set_section_badge(int section, const QString& badge, const QString& tooltip = {});
  // Opens a section and unfolds the Browser if needed.
  void reveal(int section);

 private:
  struct Section {
    QToolButton* header = nullptr;
    QWidget* content = nullptr;
    QString title;
  };
  void relayout();

  QWidget* column_ = nullptr;
  QVBoxLayout* column_layout_ = nullptr;
  std::vector<Section> sections_;
};

}  // namespace vs::app

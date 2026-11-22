#pragma once

// The drawing dialogs (L6, decision 0038), modeless EditDialogs like every
// authoring dialog: the main window previews each change (the drawing
// window redraws from the preview's evaluation) and OK commits it as one
// undo step (add_view() / update_view(), add_dimension() /
// update_dimension(), drawings.hpp).
//
//   ViewDialog       a view's name, direction, the volumes it shows (all,
//                    or a checked list), what it draws (boundaries,
//                    contents, hidden edges, connection surfaces), where
//                    its centre sits on the sheet and its scale.
//   DimensionDialog  a dimension's view, kind (horizontal, vertical,
//                    diameter), the two anchors the view offers for that
//                    kind (anchor_choices(); the second hidden for a
//                    diameter; disposable ones in parentheses and in the
//                    disposable pen's colour) and the offset of its line.
//
// Both read what to offer from the evaluation shown when they opened.

#include <memory>
#include <vector>

#include "drawings.hpp"
#include "vs/eval/engine.hpp"
#include "vs/model/document.hpp"
#include "widgets.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;

namespace vs::app {

class ViewDialog : public EditDialog {
  Q_OBJECT

 public:
  ViewDialog(const model::DrawingView& view, const model::Document& document, QWidget* parent = nullptr);
  ~ViewDialog() override;

  [[nodiscard]] model::DrawingView value() const;
  void set_value(const model::DrawingView& view);
  [[nodiscard]] bool complete() const override;

  [[nodiscard]] QComboBox& direction_box() { return *direction_; }
  [[nodiscard]] QCheckBox& all_volumes() { return *all_; }
  [[nodiscard]] QListWidget& volume_list() { return *volumes_list_; }

 private:
  model::DrawingView original_;
  QLineEdit* name_ = nullptr;
  QComboBox* direction_ = nullptr;
  QCheckBox* all_ = nullptr;
  QListWidget* volumes_list_ = nullptr;
  CheckList<Id> volumes_;
  QCheckBox* boundaries_ = nullptr;
  QCheckBox* contents_ = nullptr;
  QCheckBox* hidden_ = nullptr;
  QCheckBox* surfaces_ = nullptr;
  QDoubleSpinBox* x_ = nullptr;
  QDoubleSpinBox* y_ = nullptr;
  QDoubleSpinBox* scale_ = nullptr;
};

class DimensionDialog : public EditDialog {
  Q_OBJECT

 public:
  // `evaluation` is what the anchors are offered from (kept alive by the
  // dialog); `drawing` the dimension's drawing as authored.
  DimensionDialog(const model::DrawingDimension& dimension, const model::Drawing& drawing,
                  std::shared_ptr<const eval::Evaluation> evaluation, QWidget* parent = nullptr);
  ~DimensionDialog() override;

  [[nodiscard]] model::DrawingDimension value() const;
  void set_value(const model::DrawingDimension& dimension);
  [[nodiscard]] bool complete() const override;

  // The anchors offered now (for the chosen view and kind).
  [[nodiscard]] const std::vector<AnchorChoice>& choices() const { return choices_; }
  [[nodiscard]] QComboBox& view_box() { return *view_; }
  [[nodiscard]] QComboBox& kind_box() { return *kind_; }
  [[nodiscard]] QComboBox& from_box() { return *from_; }
  [[nodiscard]] QComboBox& to_box() { return *to_; }
  [[nodiscard]] QDoubleSpinBox& offset_box() { return *offset_; }
  // Chooses an anchor in a box by its index in choices(); false when out of range.
  bool choose(bool from, std::size_t choice);

 private:
  // Fills the anchor boxes for the chosen view and kind, keeping the
  // chosen anchors where they are still offered.
  void refill();

  model::DrawingDimension original_;
  model::Drawing drawing_;
  std::shared_ptr<const eval::Evaluation> evaluation_;
  std::vector<AnchorChoice> choices_;
  QComboBox* view_ = nullptr;
  QComboBox* kind_ = nullptr;
  QComboBox* from_ = nullptr;
  QComboBox* to_ = nullptr;
  QDoubleSpinBox* offset_ = nullptr;
  RefPicker<model::Anchor> from_picker_;
  RefPicker<model::Anchor> to_picker_;
  bool filling_ = false;
};

}  // namespace vs::app

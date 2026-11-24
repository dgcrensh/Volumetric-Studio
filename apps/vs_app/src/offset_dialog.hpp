#pragma once

// Offset (decision 0041): the options of an offset of sketch curves. A
// modeless EditDialog like Import SVG: every change previews on the sketch
// (SketchMode::preview_offset()), OK keeps it as one sketch step, Cancel
// takes it out. The curves are the sketch's selection when the dialog
// opened, grown to their whole chains while "Whole chains" is on. The main
// window wires it up (MainWindow::offset_curves()).

#include "vs/sketch/offset.hpp"
#include "widgets.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;

namespace vs::app {

class OffsetDialog : public EditDialog {
  Q_OBJECT

 public:
  explicit OffsetDialog(QWidget* parent = nullptr);

  // The distance with its side's sign (outward positive).
  [[nodiscard]] sketch::OffsetOptions options() const;
  [[nodiscard]] bool whole_chains() const;

  // The fields, for tests.
  [[nodiscard]] QDoubleSpinBox& distance() const { return *distance_; }
  // 0: outward (left of an open chain), 1: inward (right).
  [[nodiscard]] QComboBox& side() const { return *side_; }
  // 0: sharp (extend), 1: round.
  [[nodiscard]] QComboBox& corners() const { return *corners_; }
  [[nodiscard]] QCheckBox& chains() const { return *chains_; }

 private:
  QDoubleSpinBox* distance_ = nullptr;
  QComboBox* side_ = nullptr;
  QComboBox* corners_ = nullptr;
  QCheckBox* chains_ = nullptr;
};

// One line for the dialog: what the offset added.
QString describe_offset(const sketch::SketchOffset& added, std::size_t sources);

}  // namespace vs::app

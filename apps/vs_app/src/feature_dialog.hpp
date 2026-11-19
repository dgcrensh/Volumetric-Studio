#pragma once

// The dialog for every M3/M4 feature (plan, M14: "Feature dialogs for every
// M3/M4 feature with live preview").
//
// One FeatureDialog edits one feature: a sketch's plane, a datum plane,
// axis or point (each of its definitions), an extrude, revolve, sweep,
// loft, shell, fillet, chamfer, or a linear or circular pattern. Its
// pickers list only what the feature may refer to (ReferenceChoices: what
// is above it, and inside a volume only the volume's inputs). feature()
// reads the widgets back into a feature with the same id; the main window
// previews it on every change() and commits it on OK.

#include <memory>

#include "reference_choices.hpp"
#include "widgets.hpp"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace vs::app {

class FeatureDialog : public EditDialog {
  Q_OBJECT

 public:
  // `is_new`: a feature being added, whose default references (the XY
  // plane, say) may not be on offer; the first choice stands in for them.
  FeatureDialog(const feature::Feature& feature, ReferenceChoices choices, QWidget* parent = nullptr,
                bool is_new = false);
  ~FeatureDialog() override;

  // The feature as the widgets describe it: the same id and kind, the
  // edited name and definition.
  [[nodiscard]] feature::Feature feature() const;
  // Puts a feature of the same kind into the widgets.
  void set_feature(const feature::Feature& feature);
  [[nodiscard]] bool complete() const override;
  [[nodiscard]] feature::FeatureKind kind() const { return original_.kind(); }

 private:
  struct Widgets;
  void build();

  feature::Feature original_;
  bool is_new_ = false;
  ReferenceChoices choices_;
  std::unique_ptr<Widgets> w_;
};

}  // namespace vs::app

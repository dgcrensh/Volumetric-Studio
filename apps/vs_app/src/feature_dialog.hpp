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
//
// Pick in view (M16): every plane and face field, a shell's faces and a
// fillet's or chamfer's edges have a Pick button. Pressing it asks the main
// window (pickRequested()) to let the viewport pick a face or an edge; the
// pick comes back through apply_pick() as the reference the lists would
// give (picking.hpp) and fills the field. Single fields stop picking after
// one pick; a shell's faces and the edges keep picking until the button is
// pressed again. The lists stay as the fallback.

#include <functional>
#include <memory>
#include <vector>

#include "picking.hpp"
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
  [[nodiscard]] const ReferenceChoices& choices() const { return choices_; }

  // --- pick in view (M16)
  enum class PickKind { Plane, Face, Edge };
  // The fields with a Pick button, in the order they appear.
  [[nodiscard]] int pick_field_count() const { return static_cast<int>(fields_.size()); }
  [[nodiscard]] PickKind pick_kind(int field) const { return fields_.at(static_cast<std::size_t>(field)).kind; }
  [[nodiscard]] QString pick_label(int field) const { return fields_.at(static_cast<std::size_t>(field)).label; }
  // The field picking now, or -1.
  [[nodiscard]] int picking() const { return picking_; }
  // Presses a field's Pick button (as the user would); -1 stops picking.
  void begin_pick(int field);
  void end_pick() { begin_pick(-1); }
  // A pick for the field picking now: fills it and returns true, or says
  // why it can't (the pick named no plane, say) and returns false.
  bool apply_pick(const PickResolution& pick);

 signals:
  // Picking started for a field (the viewport should pick faces or edges)
  // or stopped (field -1).
  void pickRequested(int field);

 private:
  struct Widgets;
  struct PickField {
    PickKind kind = PickKind::Face;
    QString label;
    bool multiple = false;  // keeps picking after a pick
    std::function<bool(const PickResolution&)> apply;
    QPushButton* button = nullptr;
  };
  void build();
  // Lists a fillet's or chamfer's edges, touching no other field (so a
  // number being typed survives adding an edge).
  void show_edges();
  // A Pick button for a field, registered in fields_.
  QPushButton* pick_button(PickKind kind, const QString& label, bool multiple,
                           std::function<bool(const PickResolution&)> apply);

  feature::Feature original_;
  bool is_new_ = false;
  ReferenceChoices choices_;
  std::unique_ptr<Widgets> w_;
  std::vector<PickField> fields_;
  int picking_ = -1;
};

}  // namespace vs::app

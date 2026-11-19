#pragma once

// Volume authoring dialogs (plan, M14: "create a volume from a solid, mark
// keep-outs, create connection surfaces from sketch entities or datums and
// set type parameters"):
//
//   VolumeDialog       name, boundary (layout solid features, or a box
//                      between two layout points), keep-out, seed, the
//                      volume's parameters and published outputs
//   ConnectionDialog   name, patch (a layout sketch's profile, or a
//                      rectangle on a layout plane), the one or two volumes
//                      it joins, its type and the type's parameters, seam
//                      spacing
//   KeepOutDialog      a keep-out region: its cutters and the volumes it cuts
//   LinkDialog         a published output driving a layout variable or a
//                      volume parameter
//
// Each reads its widgets back into the model value (value()), keeping the
// id it was given; the main window previews and commits it like a feature.

#include <memory>

#include "vs/eval/engine.hpp"
#include "vs/model/document.hpp"
#include "widgets.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

namespace vs::app {

class VolumeDialog : public EditDialog {
  Q_OBJECT

 public:
  VolumeDialog(const model::Volume& volume, const eval::Evaluation& evaluation, QWidget* parent = nullptr);
  ~VolumeDialog() override;
  [[nodiscard]] model::Volume value() const;
  void set_value(const model::Volume& volume);
  [[nodiscard]] bool complete() const override;

  // For tests and scripts: add a parameter or output row.
  void add_parameter(const std::string& name, const QString& value);
  void add_output(const std::string& name, model::OutputQuantity quantity, double density = 1.0);

 private:
  struct Widgets;
  model::Volume original_;
  std::unique_ptr<Widgets> w_;
};

class ConnectionDialog : public EditDialog {
  Q_OBJECT

 public:
  ConnectionDialog(const model::ConnectionSurface& surface, const eval::Evaluation& evaluation,
                   QWidget* parent = nullptr);
  ~ConnectionDialog() override;
  [[nodiscard]] model::ConnectionSurface value() const;
  void set_value(const model::ConnectionSurface& surface);
  [[nodiscard]] bool complete() const override;

 private:
  struct Widgets;
  model::ConnectionSurface original_;
  std::unique_ptr<Widgets> w_;
};

class KeepOutDialog : public EditDialog {
  Q_OBJECT

 public:
  KeepOutDialog(const model::KeepOut& keep_out, const eval::Evaluation& evaluation, QWidget* parent = nullptr);
  ~KeepOutDialog() override;
  [[nodiscard]] model::KeepOut value() const;
  void set_value(const model::KeepOut& keep_out);
  [[nodiscard]] bool complete() const override;

 private:
  struct Widgets;
  model::KeepOut original_;
  std::unique_ptr<Widgets> w_;
};

class LinkDialog : public EditDialog {
  Q_OBJECT

 public:
  LinkDialog(const model::Link& link, const eval::Evaluation& evaluation, QWidget* parent = nullptr);
  ~LinkDialog() override;
  [[nodiscard]] model::Link value() const;
  void set_value(const model::Link& link);
  [[nodiscard]] bool complete() const override;

 private:
  struct Widgets;
  model::Link original_;
  std::unique_ptr<Widgets> w_;
};

}  // namespace vs::app

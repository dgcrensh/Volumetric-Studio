#pragma once

// Import SVG (S1, decision 0040): the options of an SVG import into the open
// sketch. A modeless EditDialog: every change previews on the sketch
// (SketchMode::preview_import()), OK keeps it as one sketch step, Cancel
// takes it out. The main window wires it up (MainWindow::import_svg()).

#include <QString>

#include "vs/sketch/svg.hpp"
#include "widgets.hpp"

class QCheckBox;
class QDoubleSpinBox;

namespace vs::app {

class SvgImportDialog : public EditDialog {
  Q_OBJECT

 public:
  explicit SvgImportDialog(const QString& path, QWidget* parent = nullptr);

  [[nodiscard]] QString path() const { return path_; }
  [[nodiscard]] sketch::SvgImportOptions options() const;
  void set_options(const sketch::SvgImportOptions& options);

  // The fields, for tests.
  [[nodiscard]] QDoubleSpinBox& pixels_per_inch() const { return *ppi_; }
  [[nodiscard]] QDoubleSpinBox& scale() const { return *scale_; }
  [[nodiscard]] QDoubleSpinBox& gap() const { return *gap_; }
  [[nodiscard]] QCheckBox& smooth_joints() const { return *smooth_; }
  [[nodiscard]] QCheckBox& fix() const { return *fix_; }

 private:
  QString path_;
  QDoubleSpinBox* ppi_ = nullptr;
  QDoubleSpinBox* scale_ = nullptr;
  QDoubleSpinBox* gap_ = nullptr;
  QCheckBox* smooth_ = nullptr;
  QCheckBox* fix_ = nullptr;
};

// One or two lines for the dialog: what came in, and the warnings.
QString describe_import(const sketch::SvgImport& imported);

}  // namespace vs::app

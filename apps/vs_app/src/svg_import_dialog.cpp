#include "svg_import_dialog.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>

namespace vs::app {

namespace {

QDoubleSpinBox* number(double lo, double hi, double value, int decimals, const QString& suffix) {
  auto* box = new QDoubleSpinBox;
  box->setRange(lo, hi);
  box->setDecimals(decimals);
  box->setValue(value);
  box->setSuffix(suffix);
  box->setKeyboardTracking(false);  // preview once a number is typed, not per key
  return box;
}

}  // namespace

SvgImportDialog::SvgImportDialog(const QString& path, QWidget* parent)
    : EditDialog(tr("Import SVG"), parent), path_(path) {
  const sketch::SvgImportOptions defaults;
  auto* file = new QLabel(QFileInfo(path).fileName());
  file->setToolTip(path);
  form()->addRow(tr("File"), file);
  ppi_ = number(1, 10000, defaults.pixels_per_inch, 2, tr(" px per inch"));
  ppi_->setToolTip(tr("How many px (and numbers without a unit) make an inch. A width and height "
                      "in mm, cm, in or pt set the scale instead."));
  form()->addRow(tr("Resolution"), ppi_);
  scale_ = number(0.0001, 10000, defaults.scale, 4, QString());
  form()->addRow(tr("Scale"), scale_);
  gap_ = number(0, 10, defaults.gap_mm, 4, tr(" mm"));
  gap_->setToolTip(tr("Ends closer than this are joined."));
  form()->addRow(tr("Join gaps up to"), gap_);
  smooth_ = new QCheckBox(tr("Keep smooth joints (tangent)"));
  smooth_->setChecked(defaults.smooth_joints);
  form()->addRow(QString(), smooth_);
  fix_ = new QCheckBox(tr("Fix imported geometry"));
  fix_->setChecked(defaults.fix);
  form()->addRow(QString(), fix_);
  // Fix holds everything, so tangents would only repeat it.
  connect(fix_, &QCheckBox::toggled, smooth_, [this](bool on) { smooth_->setEnabled(!on); });
  for (QWidget* w : {static_cast<QWidget*>(ppi_), static_cast<QWidget*>(scale_), static_cast<QWidget*>(gap_),
                     static_cast<QWidget*>(smooth_), static_cast<QWidget*>(fix_)}) {
    watch(w);
  }
}

sketch::SvgImportOptions SvgImportDialog::options() const {
  sketch::SvgImportOptions o;
  o.pixels_per_inch = ppi_->value();
  o.scale = scale_->value();
  o.gap_mm = gap_->value();
  o.smooth_joints = smooth_->isChecked();
  o.fix = fix_->isChecked();
  return o;
}

void SvgImportDialog::set_options(const sketch::SvgImportOptions& o) {
  ppi_->setValue(o.pixels_per_inch);
  scale_->setValue(o.scale);
  gap_->setValue(o.gap_mm);
  smooth_->setChecked(o.smooth_joints);
  fix_->setChecked(o.fix);
}

QString describe_import(const sketch::SvgImport& imported) {
  QString text = SvgImportDialog::tr("%n curve(s) imported", nullptr, static_cast<int>(imported.curves.size()));
  if (!imported.open.empty()) {
    text += SvgImportDialog::tr(", %n not on a closed loop (construction)", nullptr,
                                static_cast<int>(imported.open.size()));
  }
  text += QStringLiteral(".");
  for (const auto& d : imported.diagnostics) {
    if (d.code == "sketch.import.open") continue;  // said above
    text += QStringLiteral("\n") + qstr(d.message) + QStringLiteral(".");
  }
  return text;
}

}  // namespace vs::app

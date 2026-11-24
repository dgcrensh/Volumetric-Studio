#include "offset_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>

namespace vs::app {

OffsetDialog::OffsetDialog(QWidget* parent) : EditDialog(tr("Offset"), parent) {
  distance_ = new QDoubleSpinBox;
  distance_->setRange(0.001, 1000);
  distance_->setDecimals(3);
  distance_->setValue(1.0);
  distance_->setSuffix(tr(" mm"));
  distance_->setKeyboardTracking(false);  // preview once a number is typed, not per key
  form()->addRow(tr("Distance"), distance_);

  side_ = new QComboBox;
  side_->addItem(tr("Outward"));
  side_->addItem(tr("Inward"));
  side_->setToolTip(tr("Away from, or into, the area a closed loop encloses. An open chain's "
                       "outward side is the left of its direction: flip it if the preview is on "
                       "the wrong side."));
  form()->addRow(tr("Side"), side_);

  corners_ = new QComboBox;
  corners_->addItem(tr("Sharp (extend to meet)"));
  corners_->addItem(tr("Round"));
  corners_->setToolTip(tr("How offsets meet outside a corner. Corners beside a curve are always "
                          "round; inside corners are trimmed."));
  form()->addRow(tr("Corners"), corners_);

  chains_ = new QCheckBox(tr("Whole chains"));
  chains_->setChecked(true);
  chains_->setToolTip(tr("Offset every curve joined end to end with the selected ones."));
  form()->addRow(QString(), chains_);

  for (QWidget* w : {static_cast<QWidget*>(distance_), static_cast<QWidget*>(side_),
                     static_cast<QWidget*>(corners_), static_cast<QWidget*>(chains_)}) {
    watch(w);
  }
}

sketch::OffsetOptions OffsetDialog::options() const {
  sketch::OffsetOptions o;
  o.distance = side_->currentIndex() == 1 ? -distance_->value() : distance_->value();
  o.corners = corners_->currentIndex() == 1 ? sketch::OffsetCorners::Round : sketch::OffsetCorners::Extend;
  return o;
}

bool OffsetDialog::whole_chains() const { return chains_->isChecked(); }

QString describe_offset(const sketch::SketchOffset& added, std::size_t sources) {
  return OffsetDialog::tr("%n curve(s) offset", nullptr, static_cast<int>(sources)) + QStringLiteral(": ") +
         OffsetDialog::tr("%n curve(s) added.", nullptr, static_cast<int>(added.curves.size()));
}

}  // namespace vs::app

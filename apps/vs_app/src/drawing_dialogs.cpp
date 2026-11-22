#include "drawing_dialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalBlocker>

#include <utility>

namespace vs::app {

namespace {

using model::DimensionKind;
using model::ViewDirection;

constexpr ViewDirection kDirections[] = {ViewDirection::Front, ViewDirection::Back,  ViewDirection::Top,
                                         ViewDirection::Bottom, ViewDirection::Left, ViewDirection::Right,
                                         ViewDirection::Isometric};
constexpr DimensionKind kKinds[] = {DimensionKind::Horizontal, DimensionKind::Vertical, DimensionKind::Diameter};

QString direction_label(ViewDirection d) {
  switch (d) {
    case ViewDirection::Front: return QObject::tr("Front");
    case ViewDirection::Back: return QObject::tr("Back");
    case ViewDirection::Top: return QObject::tr("Top");
    case ViewDirection::Bottom: return QObject::tr("Bottom");
    case ViewDirection::Left: return QObject::tr("Left");
    case ViewDirection::Right: return QObject::tr("Right");
    case ViewDirection::Isometric: return QObject::tr("Isometric");
  }
  return {};
}

QString kind_label(DimensionKind k) {
  switch (k) {
    case DimensionKind::Horizontal: return QObject::tr("Horizontal");
    case DimensionKind::Vertical: return QObject::tr("Vertical");
    case DimensionKind::Diameter: return QObject::tr("Diameter");
  }
  return {};
}

QDoubleSpinBox* number(double lo, double hi, int decimals, const QString& suffix) {
  auto* s = new QDoubleSpinBox;
  s->setRange(lo, hi);
  s->setDecimals(decimals);
  s->setSuffix(suffix);
  s->setKeyboardTracking(false);
  return s;
}

// Every authored volume (generated ones come and go with their generator),
// indented under its composite.
std::vector<Choice<Id>> shown_volume_choices(const model::Document& doc) {
  std::vector<Choice<Id>> out;
  for (const auto& v : doc.volumes) {
    if (v.generated) continue;
    int depth = 0;
    for (Id p = v.parent; !p.is_null(); ++depth) {
      const model::Volume* parent = doc.find_volume(p);
      p = parent != nullptr ? parent->parent : Id{};
    }
    out.push_back({std::string(static_cast<std::size_t>(depth) * 4, ' ') + v.name, v.id});
  }
  return out;
}

}  // namespace

// --- ViewDialog -------------------------------------------------------------------------------

ViewDialog::ViewDialog(const model::DrawingView& view, const model::Document& document, QWidget* parent)
    : EditDialog(tr("Drawing view"), parent), original_(view) {
  QFormLayout* f = form();
  name_ = new QLineEdit;
  name_->setObjectName(QStringLiteral("name"));
  f->addRow(tr("Name"), name_);
  watch(name_);
  direction_ = new QComboBox;
  for (const auto d : kDirections) direction_->addItem(direction_label(d), static_cast<int>(d));
  f->addRow(tr("Looking from"), direction_);
  watch(direction_);
  all_ = new QCheckBox(tr("Every volume"));
  f->addRow(tr("Shows"), all_);
  watch(all_);
  volumes_list_ = new QListWidget;
  volumes_list_->setMaximumHeight(140);
  volumes_ = CheckList<Id>(volumes_list_, shown_volume_choices(document));
  f->addRow(QString(), volumes_list_);
  watch(volumes_list_);
  connect(all_, &QCheckBox::toggled, volumes_list_, [this](bool on) { volumes_list_->setEnabled(!on); });
  boundaries_ = new QCheckBox(tr("Boundaries (phantom lines)"));
  contents_ = new QCheckBox(tr("Contents, hidden lines removed"));
  hidden_ = new QCheckBox(tr("Hidden edges, dashed"));
  surfaces_ = new QCheckBox(tr("Connection surfaces (dash-dot)"));
  for (QCheckBox* c : {boundaries_, contents_, hidden_, surfaces_}) {
    f->addRow(QString(), c);
    watch(c);
  }
  connect(contents_, &QCheckBox::toggled, hidden_, &QCheckBox::setEnabled);
  x_ = number(-10000, 10000, 2, tr(" mm"));
  y_ = number(-10000, 10000, 2, tr(" mm"));
  scale_ = number(0.0001, 1000, 4, {});
  scale_->setToolTip(tr("Sheet millimetres per model millimetre: 0.5 is 1:2"));
  f->addRow(tr("Centre at x"), x_);
  f->addRow(tr("Centre at y"), y_);
  f->addRow(tr("Scale"), scale_);
  for (QDoubleSpinBox* s : {x_, y_, scale_}) watch(s);
  set_value(view);
}

ViewDialog::~ViewDialog() = default;

void ViewDialog::set_value(const model::DrawingView& v) {
  const QSignalBlocker block(this);
  name_->setText(qstr(v.name));
  direction_->setCurrentIndex(std::max(0, direction_->findData(static_cast<int>(v.direction))));
  all_->setChecked(v.volumes.empty());
  volumes_list_->setEnabled(!v.volumes.empty());
  volumes_.set(v.volumes);
  boundaries_->setChecked(v.boundaries);
  contents_->setChecked(v.contents);
  hidden_->setChecked(v.hidden);
  hidden_->setEnabled(v.contents);
  surfaces_->setChecked(v.surfaces);
  x_->setValue(v.x);
  y_->setValue(v.y);
  scale_->setValue(v.scale);
}

model::DrawingView ViewDialog::value() const {
  model::DrawingView v = original_;
  v.name = sstr(name_->text().trimmed());
  v.direction = static_cast<ViewDirection>(direction_->currentData().toInt());
  v.volumes = all_->isChecked() ? std::vector<Id>{} : volumes_.get();
  v.boundaries = boundaries_->isChecked();
  v.contents = contents_->isChecked();
  v.hidden = hidden_->isChecked();
  v.surfaces = surfaces_->isChecked();
  v.x = x_->value();
  v.y = y_->value();
  v.scale = scale_->value();
  return v;
}

bool ViewDialog::complete() const {
  return !name_->text().trimmed().isEmpty() && (all_->isChecked() || !volumes_.get().empty()) &&
         scale_->value() > 0;
}

// --- DimensionDialog --------------------------------------------------------------------------

DimensionDialog::DimensionDialog(const model::DrawingDimension& dimension, const model::Drawing& drawing,
                                 std::shared_ptr<const eval::Evaluation> evaluation, QWidget* parent)
    : EditDialog(tr("Dimension"), parent), original_(dimension), drawing_(drawing), evaluation_(std::move(evaluation)) {
  QFormLayout* f = form();
  view_ = new QComboBox;
  for (const auto& v : drawing_.views) view_->addItem(qstr(v.name), qstr(v.id.to_string()));
  f->addRow(tr("View"), view_);
  kind_ = new QComboBox;
  for (const auto k : kKinds) kind_->addItem(kind_label(k), static_cast<int>(k));
  f->addRow(tr("Kind"), kind_);
  from_ = new QComboBox;
  to_ = new QComboBox;
  for (QComboBox* b : {from_, to_}) {
    b->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    b->setMinimumContentsLength(36);
    b->setToolTip(tr("Anchors in parentheses are on generated contents: the dimension is disposable, "
                     "measured again after every regeneration and dropped if its anchor goes"));
  }
  f->addRow(tr("From"), from_);
  f->addRow(tr("To"), to_);
  offset_ = number(-1000, 1000, 2, tr(" mm"));
  offset_->setToolTip(tr("Sheet millimetres from the anchors to the dimension line"));
  f->addRow(tr("Offset"), offset_);
  for (QWidget* w : {static_cast<QWidget*>(from_), static_cast<QWidget*>(to_), static_cast<QWidget*>(offset_)}) {
    watch(w);
  }
  // The view and the kind change what is offered.
  const auto changed_choices = [this] {
    if (filling_) return;
    refill();
    emit changed();
  };
  connect(view_, &QComboBox::currentIndexChanged, this, changed_choices);
  connect(kind_, &QComboBox::currentIndexChanged, this, changed_choices);
  set_value(dimension);
}

DimensionDialog::~DimensionDialog() = default;

void DimensionDialog::refill() {
  const auto keep_from = from_picker_.box() != nullptr ? from_picker_.get() : std::nullopt;
  const auto keep_to = to_picker_.box() != nullptr ? to_picker_.get() : std::nullopt;
  const bool was = filling_;
  filling_ = true;
  const QSignalBlocker b1(from_);
  const QSignalBlocker b2(to_);
  choices_.clear();
  const auto view_id = Id::parse(sstr(view_->currentData().toString()));
  const model::DrawingView* view = view_id ? drawing_.find_view(*view_id) : nullptr;
  const auto kind = static_cast<DimensionKind>(kind_->currentData().toInt());
  if (view != nullptr && evaluation_) choices_ = anchor_choices(*evaluation_, *view, kind);
  std::vector<Choice<model::Anchor>> refs;
  refs.reserve(choices_.size());
  for (const auto& c : choices_) refs.push_back({c.label, c.anchor});
  from_picker_ = RefPicker<model::Anchor>(from_, refs);
  to_picker_ = RefPicker<model::Anchor>(to_, refs);
  for (std::size_t i = 0; i < choices_.size(); ++i) {
    if (!choices_[i].disposable) continue;
    for (QComboBox* b : {from_, to_}) {
      b->setItemData(static_cast<int>(i), QColor(0xb3, 0x5c, 0x00), Qt::ForegroundRole);
    }
  }
  if (keep_from) from_picker_.set(*keep_from, false);
  if (keep_to) {
    to_picker_.set(*keep_to, false);
  } else if (to_->count() > 1) {
    to_->setCurrentIndex(1);
  }
  form()->setRowVisible(to_, kind != DimensionKind::Diameter);
  filling_ = was;
}

void DimensionDialog::set_value(const model::DrawingDimension& d) {
  const QSignalBlocker block(this);
  filling_ = true;
  view_->setCurrentIndex(std::max(0, view_->findData(qstr(d.view.to_string()))));
  kind_->setCurrentIndex(std::max(0, kind_->findData(static_cast<int>(d.kind))));
  filling_ = false;
  from_picker_ = {};
  to_picker_ = {};
  refill();
  const QSignalBlocker b1(from_);
  const QSignalBlocker b2(to_);
  // A new dimension starts from the first two choices; an existing one
  // keeps its anchors, offered or not ("(missing reference)").
  const bool is_new = drawing_.find_dimension(d.id) == nullptr;
  if (!is_new) {
    from_picker_.set(d.from, true);
    if (d.kind != DimensionKind::Diameter) to_picker_.set(d.to, true);
  }
  offset_->setValue(d.offset);
}

model::DrawingDimension DimensionDialog::value() const {
  model::DrawingDimension d = original_;
  if (const auto v = Id::parse(sstr(view_->currentData().toString()))) d.view = *v;
  d.kind = static_cast<DimensionKind>(kind_->currentData().toInt());
  if (const auto a = from_picker_.get()) d.from = *a;
  if (d.kind == DimensionKind::Diameter) {
    d.to = {};
  } else if (const auto b = to_picker_.get()) {
    d.to = *b;
  }
  d.offset = offset_->value();
  return d;
}

bool DimensionDialog::complete() const {
  if (view_->currentIndex() < 0 || !from_picker_.get()) return false;
  const auto kind = static_cast<DimensionKind>(kind_->currentData().toInt());
  return kind == DimensionKind::Diameter || to_picker_.get().has_value();
}

bool DimensionDialog::choose(bool from, std::size_t choice) {
  if (choice >= choices_.size()) return false;
  QComboBox* box = from ? from_ : to_;
  box->setCurrentIndex(static_cast<int>(choice));
  return true;
}

}  // namespace vs::app

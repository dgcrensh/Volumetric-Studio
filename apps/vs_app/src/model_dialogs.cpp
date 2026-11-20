#include "model_dialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "reference_choices.hpp"

namespace vs::app {

namespace {

using feature::Param;

QStackedWidget* pages_for(QComboBox* chooser, QFormLayout* form) {
  auto* pages = new QStackedWidget;
  form->addRow(pages);
  QObject::connect(chooser, &QComboBox::currentIndexChanged, pages, &QStackedWidget::setCurrentIndex);
  return pages;
}

QFormLayout* add_page(QStackedWidget* pages) {
  auto* page = new QWidget;
  auto* f = new QFormLayout(page);
  f->setContentsMargins(0, 0, 0, 0);
  pages->addWidget(page);
  return f;
}

// Layout solid features that add material: what a boundary or cutter can use.
std::vector<Choice<Id>> adding_features(const eval::Evaluation& ev) {
  std::vector<Choice<Id>> out;
  for (const auto& c : layout_choices(ev).solid_features) {
    const auto* f = ev.document.layout.find(c.ref);
    bool adds = false;
    if (f != nullptr) {
      if (const auto* e = f->as<feature::ExtrudeFeature>()) adds = e->operation == feature::Operation::Add;
      if (const auto* r = f->as<feature::RevolveFeature>()) adds = r->operation == feature::Operation::Add;
      if (const auto* s = f->as<feature::SweepFeature>()) adds = s->operation == feature::Operation::Add;
      if (const auto* l = f->as<feature::LoftFeature>()) adds = l->operation == feature::Operation::Add;
    }
    if (adds) out.push_back(c);
  }
  return out;
}

std::vector<Choice<Id>> volume_choices(const model::Document& doc, bool keep_outs) {
  std::vector<Choice<Id>> out;
  for (const auto& v : doc.volumes) {
    if (v.keep_out == keep_outs) out.push_back({v.name, v.id});
  }
  return out;
}

constexpr model::OutputQuantity kQuantities[] = {
    model::OutputQuantity::Volume,          model::OutputQuantity::Mass,     model::OutputQuantity::SurfaceArea,
    model::OutputQuantity::CentreOfGravity, model::OutputQuantity::Envelope, model::OutputQuantity::Published};

}  // namespace

// --- volumes ---------------------------------------------------------------------------

struct VolumeDialog::Widgets {
  QLineEdit* name = nullptr;
  QComboBox* boundary_kind = nullptr;
  CheckList<Id> features;
  RefPicker<feature::PointRef> min, max;
  QCheckBox* keep_out = nullptr;
  QLineEdit* seed = nullptr;
  QTableWidget* parameters = nullptr;
  std::vector<Id> parameter_ids;
  QTableWidget* outputs = nullptr;
  std::vector<Id> output_ids;
};

VolumeDialog::VolumeDialog(const model::Volume& volume, const eval::Evaluation& ev, QWidget* parent)
    : EditDialog(tr("Volume"), parent), original_(volume), w_(std::make_unique<Widgets>()) {
  auto& w = *w_;
  QFormLayout* f = form();
  w.name = new QLineEdit;
  f->addRow(tr("Name"), w.name);
  watch(w.name);
  w.boundary_kind = new QComboBox;
  w.boundary_kind->addItems({tr("Layout solid features"), tr("Box between two layout points")});
  f->addRow(tr("Boundary"), w.boundary_kind);
  watch(w.boundary_kind);
  auto* pages = pages_for(w.boundary_kind, f);
  {
    auto* p = add_page(pages);
    auto* list = new QListWidget;
    list->setMinimumHeight(90);
    p->addRow(tr("Union of"), list);
    w.features = CheckList<Id>(list, adding_features(ev));
    watch(list);
  }
  {
    auto* p = add_page(pages);
    const auto choices = layout_choices(ev);
    auto* lo = new QComboBox;
    auto* hi = new QComboBox;
    p->addRow(tr("Minimum corner"), lo);
    p->addRow(tr("Maximum corner"), hi);
    w.min = RefPicker<feature::PointRef>(lo, choices.points);
    w.max = RefPicker<feature::PointRef>(hi, choices.points);
    watch(lo);
    watch(hi);
  }
  w.keep_out = new QCheckBox(tr("Keep-out: reserve this space as empty"));
  f->addRow(QString(), w.keep_out);
  watch(w.keep_out);
  w.seed = new QLineEdit;
  w.seed->setValidator(nullptr);
  f->addRow(tr("Seed"), w.seed);
  watch(w.seed);

  auto table = [&](const QStringList& headers, const QString& label, auto add_row) {
    auto* t = new QTableWidget(0, static_cast<int>(headers.size()));
    t->setHorizontalHeaderLabels(headers);
    t->horizontalHeader()->setStretchLastSection(true);
    t->verticalHeader()->setVisible(false);
    t->setMinimumHeight(100);
    auto* box = new QWidget;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(t);
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"));
    auto* remove = new QPushButton(tr("Remove"));
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    v->addLayout(buttons);
    f->addRow(label, box);
    connect(add, &QPushButton::clicked, this, add_row);
    connect(remove, &QPushButton::clicked, this, [this, t] {
      const int row = t->currentRow();
      if (row < 0) return;
      auto& ids = t == w_->parameters ? w_->parameter_ids : w_->output_ids;
      ids.erase(ids.begin() + row);
      t->removeRow(row);
      emit changed();
    });
    watch(t);
    return t;
  };
  w.parameters = table({tr("Name"), tr("Value")}, tr("Parameters"), [this] {
    add_parameter(fmt::format("p{}", w_->parameter_ids.size() + 1), QStringLiteral("0"));
  });
  w.outputs = table({tr("Name"), tr("Quantity"), tr("Density g/cm³")}, tr("Published outputs"), [this] {
    add_output(fmt::format("output{}", w_->output_ids.size() + 1), model::OutputQuantity::Volume);
  });
  set_value(volume);
}

VolumeDialog::~VolumeDialog() = default;

void VolumeDialog::add_parameter(const std::string& name, const QString& value) {
  auto& w = *w_;
  const int row = w.parameters->rowCount();
  w.parameters->insertRow(row);
  w.parameters->setItem(row, 0, new QTableWidgetItem(qstr(name)));
  w.parameters->setItem(row, 1, new QTableWidgetItem(value));
  w.parameter_ids.push_back(Id::random());
  emit changed();
}

void VolumeDialog::add_output(const std::string& name, model::OutputQuantity quantity, double density) {
  auto& w = *w_;
  const int row = w.outputs->rowCount();
  w.outputs->insertRow(row);
  w.outputs->setItem(row, 0, new QTableWidgetItem(qstr(name)));
  auto* q = new QComboBox;
  for (const auto k : kQuantities) q->addItem(qstr(model::to_string(k)), static_cast<int>(k));
  q->setCurrentIndex(q->findData(static_cast<int>(quantity)));
  connect(q, &QComboBox::currentIndexChanged, this, &EditDialog::changed);
  w.outputs->setCellWidget(row, 1, q);
  w.outputs->setItem(row, 2, new QTableWidgetItem(QString::number(density)));
  w.output_ids.push_back(Id::random());
  emit changed();
}

void VolumeDialog::set_value(const model::Volume& v) {
  auto& w = *w_;
  const QSignalBlocker block(this);
  w.name->setText(qstr(v.name));
  if (const auto* fb = std::get_if<model::FeatureBoundary>(&v.boundary)) {
    w.boundary_kind->setCurrentIndex(0);
    w.features.set(fb->features);
  } else if (const auto* bb = std::get_if<model::BoxBoundary>(&v.boundary)) {
    w.boundary_kind->setCurrentIndex(1);
    w.min.set(bb->min);
    w.max.set(bb->max);
  }
  w.keep_out->setChecked(v.keep_out);
  w.seed->setText(QString::number(v.seed));
  w.parameters->setRowCount(0);
  w.parameter_ids.clear();
  for (const auto& p : v.parameters) {
    add_parameter(p.name, param_text(p.value, ParamUnit::Length));
    w.parameter_ids.back() = p.id;
  }
  w.outputs->setRowCount(0);
  w.output_ids.clear();
  for (const auto& o : v.outputs) {
    add_output(o.name, o.quantity, o.density);
    w.output_ids.back() = o.id;
  }
}

model::Volume VolumeDialog::value() const {
  const auto& w = *w_;
  model::Volume v = original_;
  v.name = sstr(w.name->text().trimmed());
  if (w.boundary_kind->currentIndex() == 0) {
    v.boundary = model::FeatureBoundary{w.features.get()};
  } else {
    v.boundary = model::BoxBoundary{w.min.get().value_or(feature::PointRef{}),
                                    w.max.get().value_or(feature::PointRef{})};
  }
  v.keep_out = w.keep_out->isChecked();
  v.seed = w.seed->text().trimmed().toULongLong();
  v.parameters.clear();
  for (int r = 0; r < w.parameters->rowCount(); ++r) {
    const auto* name = w.parameters->item(r, 0);
    const auto* value = w.parameters->item(r, 1);
    model::VolumeParameter p;
    p.id = w.parameter_ids[static_cast<std::size_t>(r)];
    p.name = name != nullptr ? sstr(name->text().trimmed()) : std::string();
    p.value = (value != nullptr ? parse_param(value->text(), ParamUnit::Length) : std::nullopt).value_or(Param{});
    v.parameters.push_back(std::move(p));
  }
  v.outputs.clear();
  for (int r = 0; r < w.outputs->rowCount(); ++r) {
    model::PublishedOutput o;
    o.id = w.output_ids[static_cast<std::size_t>(r)];
    if (const auto* name = w.outputs->item(r, 0)) o.name = sstr(name->text().trimmed());
    if (const auto* q = qobject_cast<QComboBox*>(w.outputs->cellWidget(r, 1))) {
      o.quantity = static_cast<model::OutputQuantity>(q->currentData().toInt());
    }
    if (const auto* d = w.outputs->item(r, 2)) o.density = d->text().toDouble();
    v.outputs.push_back(std::move(o));
  }
  return v;
}

bool VolumeDialog::complete() const {
  const auto& w = *w_;
  if (w.name->text().trimmed().isEmpty()) return false;
  if (w.boundary_kind->currentIndex() == 0) return !w.features.get().empty();
  return w.min.get().has_value() && w.max.get().has_value();
}

// --- connection surfaces ------------------------------------------------------------------

struct ConnectionDialog::Widgets {
  QLineEdit* name = nullptr;
  QComboBox* patch_kind = nullptr;
  RefPicker<Id> sketch;
  RefPicker<feature::PlaneRef> plane;
  ParamEdit *u = nullptr, *v = nullptr, *width = nullptr, *height = nullptr;
  RefPicker<feature::AxisRef> axis;
  ParamEdit *radius = nullptr, *arc_start = nullptr, *sweep = nullptr, *along = nullptr,
            *length = nullptr;
  RefPicker<Id> first, second;
  QComboBox* type = nullptr;
  ParamEdit* gap = nullptr;
  QComboBox* holes_kind = nullptr;
  ParamEdit *count = nullptr, *diameter = nullptr, *start_angle = nullptr;
  ParamEdit *columns = nullptr, *rows = nullptr, *pitch_x = nullptr, *pitch_y = nullptr;
  ParamEdit *hole_u = nullptr, *hole_v = nullptr;
  QComboBox* thread = nullptr;
  ParamEdit* min_wall = nullptr;
  QDoubleSpinBox* spacing = nullptr;
};

ConnectionDialog::ConnectionDialog(const model::ConnectionSurface& surface, const eval::Evaluation& ev,
                                   QWidget* parent)
    : EditDialog(tr("Connection surface"), parent), original_(surface), w_(std::make_unique<Widgets>()) {
  auto& w = *w_;
  QFormLayout* f = form();
  const ReferenceChoices layout = layout_choices(ev);
  auto param = [&](QFormLayout* form, const QString& label, ParamUnit unit) {
    auto* e = new ParamEdit(unit);
    form->addRow(label, e);
    watch(e);
    return e;
  };
  w.name = new QLineEdit;
  f->addRow(tr("Name"), w.name);
  watch(w.name);

  w.patch_kind = new QComboBox;
  w.patch_kind->addItems({tr("A layout sketch's profile"), tr("A rectangle on a layout plane"),
                          tr("Part of a cylinder round a layout axis")});
  f->addRow(tr("Patch"), w.patch_kind);
  watch(w.patch_kind);
  auto* patch_pages = pages_for(w.patch_kind, f);
  {
    auto* p = add_page(patch_pages);
    auto* box = new QComboBox;
    p->addRow(tr("Sketch"), box);
    w.sketch = RefPicker<Id>(box, layout.sketches);
    watch(box);
  }
  {
    auto* p = add_page(patch_pages);
    auto* box = new QComboBox;
    p->addRow(tr("Plane"), box);
    w.plane = RefPicker<feature::PlaneRef>(box, layout.planes);
    watch(box);
    w.u = param(p, tr("Centre u"), ParamUnit::Length);
    w.v = param(p, tr("Centre v"), ParamUnit::Length);
    w.width = param(p, tr("Width"), ParamUnit::Length);
    w.height = param(p, tr("Height"), ParamUnit::Length);
  }
  {
    auto* p = add_page(patch_pages);
    auto* box = new QComboBox;
    p->addRow(tr("Axis"), box);
    w.axis = RefPicker<feature::AxisRef>(box, layout.axes);
    watch(box);
    w.radius = param(p, tr("Radius"), ParamUnit::Length);
    w.arc_start = param(p, tr("Start angle"), ParamUnit::Angle);
    w.sweep = param(p, tr("Sweep (a full turn: a band)"), ParamUnit::Angle);
    w.along = param(p, tr("Start along the axis"), ParamUnit::Length);
    w.length = param(p, tr("Length"), ParamUnit::Length);
  }

  auto* first = new QComboBox;
  auto* second = new QComboBox;
  f->addRow(tr("Joins"), first);
  f->addRow(tr("and"), second);
  w.first = RefPicker<Id>(first, volume_choices(ev.document, false));
  w.second = RefPicker<Id>(second, volume_choices(ev.document, false), true, tr("(the outside)"));
  watch(first);
  watch(second);

  w.type = new QComboBox;
  w.type->addItems({tr("Bonded"), tr("Clearance"), tr("Bolted flange")});
  f->addRow(tr("Type"), w.type);
  watch(w.type);
  auto* type_pages = pages_for(w.type, f);
  {
    auto* p = add_page(type_pages);
    p->addRow(new QLabel(tr("Faces fused together: no parameters.")));
  }
  {
    auto* p = add_page(type_pages);
    w.gap = param(p, tr("Gap"), ParamUnit::Length);
  }
  {
    auto* p = add_page(type_pages);
    w.holes_kind = new QComboBox;
    w.holes_kind->addItems({tr("Bolt circle"), tr("Grid")});
    p->addRow(tr("Holes"), w.holes_kind);
    watch(w.holes_kind);
    auto* hole_pages = pages_for(w.holes_kind, p);
    auto* circle = add_page(hole_pages);
    w.count = param(circle, tr("Count"), ParamUnit::Count);
    w.diameter = param(circle, tr("Pitch circle diameter"), ParamUnit::Length);
    w.start_angle = param(circle, tr("Start angle"), ParamUnit::Angle);
    auto* grid = add_page(hole_pages);
    w.columns = param(grid, tr("Columns"), ParamUnit::Count);
    w.rows = param(grid, tr("Rows"), ParamUnit::Count);
    w.pitch_x = param(grid, tr("Pitch x"), ParamUnit::Length);
    w.pitch_y = param(grid, tr("Pitch y"), ParamUnit::Length);
    w.hole_u = param(p, tr("Pattern centre u"), ParamUnit::Length);
    w.hole_v = param(p, tr("Pattern centre v"), ParamUnit::Length);
    w.thread = new QComboBox;
    for (const auto& t : model::thread_sizes()) w.thread->addItem(qstr(t.name));
    p->addRow(tr("Thread"), w.thread);
    watch(w.thread);
    w.min_wall = param(p, tr("Minimum wall"), ParamUnit::Length);
  }
  w.spacing = new QDoubleSpinBox;
  w.spacing->setRange(0.0, 1000.0);
  w.spacing->setDecimals(3);
  w.spacing->setSuffix(QStringLiteral(" mm"));
  w.spacing->setSpecialValueText(tr("derived"));
  f->addRow(tr("Seam spacing"), w.spacing);
  watch(w.spacing);
  set_value(surface);
}

ConnectionDialog::~ConnectionDialog() = default;

void ConnectionDialog::set_value(const model::ConnectionSurface& s) {
  auto& w = *w_;
  const QSignalBlocker block(this);
  w.name->setText(qstr(s.name));
  if (const auto* sp = std::get_if<model::SketchPatch>(&s.geometry)) {
    w.patch_kind->setCurrentIndex(0);
    if (!sp->sketch.is_null()) w.sketch.set(sp->sketch);
  }
  // The rectangle's fields show their values (or defaults) either way.
  const auto* dp = std::get_if<model::DatumPatch>(&s.geometry);
  const model::DatumPatch rect = dp != nullptr ? *dp : model::DatumPatch{};
  if (dp != nullptr) {
    w.patch_kind->setCurrentIndex(1);
    w.plane.set(dp->plane);
  }
  w.u->set_param(rect.u);
  w.v->set_param(rect.v);
  w.width->set_param(rect.width);
  w.height->set_param(rect.height);
  const auto* cp = std::get_if<model::CylinderPatch>(&s.geometry);
  const model::CylinderPatch cyl = cp != nullptr ? *cp : model::CylinderPatch{};
  if (cp != nullptr) {
    w.patch_kind->setCurrentIndex(2);
    w.axis.set(cp->axis);
  }
  w.radius->set_param(cyl.radius);
  w.arc_start->set_param(cyl.start_angle);
  w.sweep->set_param(cyl.sweep);
  w.along->set_param(cyl.start);
  w.length->set_param(cyl.length);
  if (!s.volumes.empty()) w.first.set(s.volumes[0]);
  if (s.volumes.size() > 1) {
    w.second.set(s.volumes[1]);
  } else {
    w.second.set_none();
  }
  w.type->setCurrentIndex(static_cast<int>(s.params.index()));
  const auto* c = std::get_if<model::ClearanceParams>(&s.params);
  w.gap->set_param((c != nullptr ? *c : model::ClearanceParams{}).gap);
  const auto* b = std::get_if<model::BoltedFlangeParams>(&s.params);
  const model::BoltedFlangeParams bolts = b != nullptr ? *b : model::BoltedFlangeParams{};
  const auto* circle = std::get_if<model::BoltCircle>(&bolts.holes.layout);
  const auto* grid = std::get_if<model::BoltGrid>(&bolts.holes.layout);
  w.holes_kind->setCurrentIndex(grid != nullptr ? 1 : 0);
  const model::BoltCircle cc = circle != nullptr ? *circle : model::BoltCircle{};
  const model::BoltGrid gg = grid != nullptr ? *grid : model::BoltGrid{};
  w.count->set_param(cc.count);
  w.diameter->set_param(cc.diameter);
  w.start_angle->set_param(cc.start_angle);
  w.columns->set_param(gg.columns);
  w.rows->set_param(gg.rows);
  w.pitch_x->set_param(gg.pitch_x);
  w.pitch_y->set_param(gg.pitch_y);
  w.hole_u->set_param(bolts.holes.u);
  w.hole_v->set_param(bolts.holes.v);
  w.thread->setCurrentIndex(std::max(0, w.thread->findText(qstr(bolts.thread))));
  w.min_wall->set_param(bolts.min_wall);
  w.spacing->setValue(s.seam.spacing);
}

model::ConnectionSurface ConnectionDialog::value() const {
  const auto& w = *w_;
  auto p = [](const ParamEdit* e) { return e->param().value_or(Param{}); };
  model::ConnectionSurface s = original_;
  s.name = sstr(w.name->text().trimmed());
  if (w.patch_kind->currentIndex() == 0) {
    s.geometry = model::SketchPatch{w.sketch.get().value_or(Id{})};
  } else if (w.patch_kind->currentIndex() == 2) {
    s.geometry = model::CylinderPatch{w.axis.get().value_or(feature::PrincipalAxis::Z), p(w.radius),
                                      p(w.arc_start), p(w.sweep), p(w.along), p(w.length)};
  } else {
    s.geometry = model::DatumPatch{w.plane.get().value_or(feature::PrincipalPlane::XY), p(w.u), p(w.v),
                                   p(w.width), p(w.height)};
  }
  s.volumes.clear();
  if (const auto a = w.first.get()) s.volumes.push_back(*a);
  if (const auto b = w.second.get()) s.volumes.push_back(*b);
  switch (w.type->currentIndex()) {
    case 0: s.params = model::BondedParams{}; break;
    case 1: s.params = model::ClearanceParams{p(w.gap)}; break;
    default: {
      model::BoltedFlangeParams bolts;
      if (w.holes_kind->currentIndex() == 0) {
        bolts.holes.layout = model::BoltCircle{p(w.count), p(w.diameter), p(w.start_angle)};
      } else {
        bolts.holes.layout = model::BoltGrid{p(w.columns), p(w.rows), p(w.pitch_x), p(w.pitch_y)};
      }
      bolts.holes.u = p(w.hole_u);
      bolts.holes.v = p(w.hole_v);
      bolts.thread = sstr(w.thread->currentText());
      bolts.min_wall = p(w.min_wall);
      s.params = bolts;
    }
  }
  s.seam.spacing = w.spacing->value();
  return s;
}

bool ConnectionDialog::complete() const {
  const auto& w = *w_;
  if (w.name->text().trimmed().isEmpty() || !w.first.get()) return false;
  if (w.patch_kind->currentIndex() == 0) return w.sketch.get().has_value();
  if (w.patch_kind->currentIndex() == 2) {
    return w.axis.get() && w.radius->param() && w.arc_start->param() && w.sweep->param() &&
           w.along->param() && w.length->param();
  }
  return w.plane.get() && w.u->param() && w.v->param() && w.width->param() && w.height->param();
}

// --- keep-out regions --------------------------------------------------------------------------

struct KeepOutDialog::Widgets {
  QLineEdit* name = nullptr;
  CheckList<Id> cutters;
  CheckList<Id> volumes;
};

KeepOutDialog::KeepOutDialog(const model::KeepOut& keep_out, const eval::Evaluation& ev, QWidget* parent)
    : EditDialog(tr("Keep-out region"), parent), original_(keep_out), w_(std::make_unique<Widgets>()) {
  auto& w = *w_;
  QFormLayout* f = form();
  w.name = new QLineEdit;
  f->addRow(tr("Name"), w.name);
  watch(w.name);
  auto cutters = adding_features(ev);
  for (const auto& c : volume_choices(ev.document, true)) cutters.push_back({"Keep-out volume " + c.label, c.ref});
  auto* cl = new QListWidget;
  f->addRow(tr("Cut with"), cl);
  w.cutters = CheckList<Id>(cl, std::move(cutters));
  watch(cl);
  auto* vl = new QListWidget;
  f->addRow(tr("From the boundaries of"), vl);
  w.volumes = CheckList<Id>(vl, volume_choices(ev.document, false));
  watch(vl);
  set_value(keep_out);
}

KeepOutDialog::~KeepOutDialog() = default;

void KeepOutDialog::set_value(const model::KeepOut& k) {
  const QSignalBlocker block(this);
  w_->name->setText(qstr(k.name));
  w_->cutters.set(k.cutters);
  w_->volumes.set(k.volumes);
}

model::KeepOut KeepOutDialog::value() const {
  model::KeepOut k = original_;
  k.name = sstr(w_->name->text().trimmed());
  k.cutters = w_->cutters.get();
  k.volumes = w_->volumes.get();
  return k;
}

bool KeepOutDialog::complete() const { return !w_->cutters.get().empty() && !w_->volumes.get().empty(); }

// --- links ----------------------------------------------------------------------------------

struct LinkDialog::Widgets {
  RefPicker<model::PublishedOutputRef> source;
  RefPicker<Id> target;
};

LinkDialog::LinkDialog(const model::Link& link, const eval::Evaluation& ev, QWidget* parent)
    : EditDialog(tr("Link"), parent), original_(link), w_(std::make_unique<Widgets>()) {
  auto& w = *w_;
  QFormLayout* f = form();
  std::vector<Choice<model::PublishedOutputRef>> sources;
  std::vector<Choice<Id>> targets;
  for (const auto& var : ev.document.layout.variables().variables()) {
    targets.push_back({"Layout variable " + var.name, var.id});
  }
  for (const auto& v : ev.document.volumes) {
    for (const auto& o : v.outputs) {
      if (model::is_scalar(o.quantity)) sources.push_back({v.name + ": " + o.name, {v.id, o.id}});
    }
    for (const auto& p : v.parameters) targets.push_back({v.name + " parameter " + p.name, p.id});
  }
  auto* s = new QComboBox;
  auto* t = new QComboBox;
  f->addRow(tr("Published output"), s);
  f->addRow(tr("Drives"), t);
  w.source = RefPicker<model::PublishedOutputRef>(s, std::move(sources));
  w.target = RefPicker<Id>(t, std::move(targets));
  watch(s);
  watch(t);
  set_value(link);
}

LinkDialog::~LinkDialog() = default;

void LinkDialog::set_value(const model::Link& l) {
  const QSignalBlocker block(this);
  if (!l.source.volume.is_null()) w_->source.set(l.source);
  if (!l.target.is_null()) w_->target.set(l.target);
}

model::Link LinkDialog::value() const {
  model::Link l = original_;
  if (const auto s = w_->source.get()) l.source = *s;
  if (const auto t = w_->target.get()) l.target = *t;
  return l;
}

bool LinkDialog::complete() const { return w_->source.get() && w_->target.get(); }

}  // namespace vs::app

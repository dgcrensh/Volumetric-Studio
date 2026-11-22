#include "feature_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "authoring.hpp"

namespace vs::app {

using namespace feature;

namespace {

// A combo box over fixed values.
template <typename E>
void fill_enum(QComboBox* box, std::initializer_list<std::pair<E, const char*>> items) {
  for (const auto& [value, label] : items) box->addItem(QString::fromUtf8(label), static_cast<int>(value));
}
template <typename E>
E enum_of(const QComboBox* box) {
  return static_cast<E>(box->currentData().toInt());
}
template <typename E>
void set_enum(QComboBox* box, E value) {
  box->setCurrentIndex(box->findData(static_cast<int>(value)));
}

}  // namespace

struct FeatureDialog::Widgets {
  QLineEdit* name = nullptr;
  // Shared by several kinds.
  QComboBox* kind_of_definition = nullptr;  // datums: which definition
  QStackedWidget* pages = nullptr;
  RefPicker<PlaneRef> plane, plane2, plane3;
  RefPicker<AxisRef> axis;
  RefPicker<PointRef> point_a, point_b, point_c;
  ParamEdit* p1 = nullptr;  // distance, depth, thickness, radius, count, x
  ParamEdit* p2 = nullptr;  // angle, spacing, draft, y
  ParamEdit* p3 = nullptr;  // z
  ParamEdit* angle = nullptr;
  QComboBox* operation = nullptr;
  QComboBox* extent = nullptr;
  QComboBox* orientation = nullptr;
  QCheckBox* flag = nullptr;  // reverse, ruled, outward
  RefPicker<Id> sketch, sketch2;
  CheckList<Id> sketches;    // loft sections
  CheckList<Id> features;    // pattern seeds
  CheckList<FaceRef> faces;  // shell: open faces
  // Fillet and chamfer edges.
  RefPicker<FaceRef> edge_face, edge_other;
  QListWidget* edge_list = nullptr;
  std::vector<EdgeRef> edges;
  // Datum-specific pages share the pickers above per page: keep page pickers apart.
  RefPicker<PlaneRef> page_plane_a, page_plane_b;
  RefPicker<AxisRef> page_axis;
  RefPicker<PointRef> page_point;
  ParamEdit* page_angle = nullptr;
};

FeatureDialog::FeatureDialog(const Feature& f, ReferenceChoices choices, QWidget* parent, bool is_new)
    : EditDialog(QString::fromStdString(kind_title(f.kind())), parent),
      original_(f),
      is_new_(is_new),
      choices_(std::move(choices)),
      w_(std::make_unique<Widgets>()) {
  build();
  set_feature(f);
  is_new_ = false;  // later set_feature() calls keep what they're given
  connect(this, &EditDialog::changed, this, [this] {
    set_message(complete() ? QString() : tr("Choose every reference and enter every number."), true);
  });
}

FeatureDialog::~FeatureDialog() = default;

void FeatureDialog::build() {
  auto& w = *w_;
  QFormLayout* f = form();
  w.name = new QLineEdit;
  w.name->setPlaceholderText(qstr(kind_title(original_.kind())));
  f->addRow(tr("Name"), w.name);
  watch(w.name);

  auto combo = [&](const QString& label) {
    auto* box = new QComboBox;
    f->addRow(label, box);
    watch(box);
    return box;
  };
  auto param = [&](const QString& label, ParamUnit unit) {
    auto* e = new ParamEdit(unit);
    f->addRow(label, e);
    watch(e);
    return e;
  };
  auto check = [&](const QString& label) {
    auto* c = new QCheckBox(label);
    f->addRow(QString(), c);
    watch(c);
    return c;
  };
  auto list = [&](const QString& label) {
    auto* l = new QListWidget;
    l->setMinimumHeight(110);
    f->addRow(label, l);
    watch(l);
    return l;
  };
  auto operation = [&] {
    w.operation = combo(tr("Operation"));
    fill_enum<Operation>(w.operation, {{Operation::Add, "Add material"}, {Operation::Cut, "Cut material"}});
  };
  // A page of a datum's definitions.
  auto page = [&](const QString& title) {
    auto* p = new QWidget;
    auto* pf = new QFormLayout(p);
    pf->setContentsMargins(0, 0, 0, 0);
    w.pages->addWidget(p);
    w.kind_of_definition->addItem(title);
    return pf;
  };
  auto page_combo = [&](QFormLayout* pf, const QString& label) {
    auto* box = new QComboBox;
    pf->addRow(label, box);
    watch(box);
    return box;
  };
  auto page_param = [&](QFormLayout* pf, const QString& label, ParamUnit unit) {
    auto* e = new ParamEdit(unit);
    pf->addRow(label, e);
    watch(e);
    return e;
  };
  // M16: a combo with a Pick button beside it.
  auto with_pick = [&](QComboBox* box, QPushButton* button) {
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(box, 1);
    h->addWidget(button);
    return row;
  };
  auto plane_apply = [](RefPicker<PlaneRef>* target) {
    return [target](const PickResolution& r) {
      if (!r.plane) return false;
      target->set(*r.plane);
      return true;
    };
  };
  auto face_apply = [](RefPicker<FaceRef>* target) {
    return [target](const PickResolution& r) {
      if (!r.face) return false;
      target->set(*r.face);
      return true;
    };
  };
  auto plane_combo = [&](const QString& label, RefPicker<PlaneRef>* target) {
    auto* box = new QComboBox;
    watch(box);
    f->addRow(label, with_pick(box, pick_button(PickKind::Plane, label, false, plane_apply(target))));
    return box;
  };
  auto page_plane_combo = [&](QFormLayout* pf, const QString& label, RefPicker<PlaneRef>* target) {
    auto* box = new QComboBox;
    watch(box);
    pf->addRow(label, with_pick(box, pick_button(PickKind::Plane, label, false, plane_apply(target))));
    return box;
  };
  auto face_combo = [&](const QString& label, RefPicker<FaceRef>* target) {
    auto* box = new QComboBox;
    watch(box);
    f->addRow(label, with_pick(box, pick_button(PickKind::Face, label, false, face_apply(target))));
    return box;
  };
  auto datum_pages = [&] {
    w.kind_of_definition = combo(tr("Defined by"));
    w.pages = new QStackedWidget;
    f->addRow(w.pages);
    connect(w.kind_of_definition, &QComboBox::currentIndexChanged, w.pages, &QStackedWidget::setCurrentIndex);
    // A Pick button on the page being left can't stay pressed (H7).
    connect(w.kind_of_definition, &QComboBox::currentIndexChanged, this, [this] { end_pick(); });
  };

  switch (original_.kind()) {
    case FeatureKind::Sketch:
      w.plane = RefPicker<PlaneRef>(plane_combo(tr("Plane"), &w.plane), choices_.planes);
      break;
    case FeatureKind::DatumPlane: {
      datum_pages();
      auto* a = page(tr("Offset from a plane"));
      w.plane = RefPicker<PlaneRef>(page_plane_combo(a, tr("Plane"), &w.plane), choices_.planes);
      w.p1 = page_param(a, tr("Distance"), ParamUnit::Length);
      auto* b = page(tr("Three points"));
      w.point_a = RefPicker<PointRef>(page_combo(b, tr("Origin")), choices_.points);
      w.point_b = RefPicker<PointRef>(page_combo(b, tr("Towards (x axis)")), choices_.points);
      w.point_c = RefPicker<PointRef>(page_combo(b, tr("Third point")), choices_.points);
      auto* c = page(tr("Axis and angle"));
      w.page_axis = RefPicker<AxisRef>(page_combo(c, tr("Axis")), choices_.axes);
      w.page_plane_a = RefPicker<PlaneRef>(page_plane_combo(c, tr("Base plane"), &w.page_plane_a), choices_.planes);
      w.page_angle = page_param(c, tr("Angle"), ParamUnit::Angle);
      break;
    }
    case FeatureKind::DatumAxis: {
      datum_pages();
      auto* a = page(tr("Two points"));
      w.point_a = RefPicker<PointRef>(page_combo(a, tr("From")), choices_.points);
      w.point_b = RefPicker<PointRef>(page_combo(a, tr("To")), choices_.points);
      auto* b = page(tr("Where two planes meet"));
      w.page_plane_a = RefPicker<PlaneRef>(page_plane_combo(b, tr("First plane"), &w.page_plane_a), choices_.planes);
      w.page_plane_b = RefPicker<PlaneRef>(page_plane_combo(b, tr("Second plane"), &w.page_plane_b), choices_.planes);
      auto* c = page(tr("Along an existing axis"));
      w.page_axis = RefPicker<AxisRef>(page_combo(c, tr("Axis")), choices_.axes);
      break;
    }
    case FeatureKind::DatumPoint: {
      datum_pages();
      auto* a = page(tr("Coordinates"));
      w.p1 = page_param(a, tr("X"), ParamUnit::Length);
      w.p2 = page_param(a, tr("Y"), ParamUnit::Length);
      w.p3 = page_param(a, tr("Z"), ParamUnit::Length);
      auto* b = page(tr("At a point"));
      w.page_point = RefPicker<PointRef>(page_combo(b, tr("Point")), choices_.points);
      auto* c = page(tr("Where an axis meets a plane"));
      w.page_axis = RefPicker<AxisRef>(page_combo(c, tr("Axis")), choices_.axes);
      w.page_plane_a = RefPicker<PlaneRef>(page_plane_combo(c, tr("Plane"), &w.page_plane_a), choices_.planes);
      break;
    }
    case FeatureKind::Extrude:
      w.sketch = RefPicker<Id>(combo(tr("Sketch")), choices_.sketches);
      operation();
      w.extent = combo(tr("Extent"));
      fill_enum<Extent>(w.extent, {{Extent::Blind, "Blind (a depth)"},
                                   {Extent::ThroughAll, "Through all"},
                                   {Extent::UpToPlane, "Up to a plane"},
                                   {Extent::Symmetric, "Symmetric (a depth in total)"}});
      w.p1 = param(tr("Depth"), ParamUnit::Length);
      w.plane = RefPicker<PlaneRef>(plane_combo(tr("Up to"), &w.plane), choices_.planes);
      w.p2 = param(tr("Draft"), ParamUnit::Angle);
      w.flag = check(tr("Reverse direction"));
      break;
    case FeatureKind::Revolve:
      w.sketch = RefPicker<Id>(combo(tr("Sketch")), choices_.sketches);
      operation();
      w.axis = RefPicker<AxisRef>(combo(tr("Axis")), choices_.axes);
      w.angle = param(tr("Angle"), ParamUnit::Angle);
      w.flag = check(tr("Reverse direction"));
      break;
    case FeatureKind::Sweep:
      w.sketch = RefPicker<Id>(combo(tr("Profile sketch")), choices_.sketches);
      w.sketch2 = RefPicker<Id>(combo(tr("Path sketch")), choices_.sketches);
      operation();
      w.orientation = combo(tr("Profile"));
      fill_enum<SweepOrientation>(w.orientation, {{SweepOrientation::FollowPath, "Turns with the path"},
                                                  {SweepOrientation::Fixed, "Keeps its orientation"}});
      break;
    case FeatureKind::Loft:
      w.sketches = CheckList<Id>(list(tr("Sections, in order")), choices_.sketches);
      operation();
      w.flag = check(tr("Ruled (straight between sections)"));
      break;
    case FeatureKind::Shell:
      w.faces = CheckList<FaceRef>(list(tr("Faces to open")), choices_.faces);
      f->addRow(QString(), pick_button(PickKind::Face, tr("Faces to open"), true, [this](const PickResolution& r) {
                  if (!r.face) return false;
                  auto open = w_->faces.get();
                  if (std::find(open.begin(), open.end(), *r.face) == open.end()) open.push_back(*r.face);
                  w_->faces.set(open);
                  emit changed();
                  return true;
                }));
      w.p1 = param(tr("Wall thickness"), ParamUnit::Length);
      w.flag = check(tr("Walls grow outwards"));
      break;
    case FeatureKind::Fillet:
    case FeatureKind::Chamfer: {
      w.edge_face = RefPicker<FaceRef>(face_combo(tr("Edges of face"), &w.edge_face), choices_.faces);
      w.edge_other = RefPicker<FaceRef>(face_combo(tr("Where it meets"), &w.edge_other), choices_.faces, true,
                                        tr("(every edge of the face)"));
      auto* row = new QWidget;
      auto* h = new QHBoxLayout(row);
      h->setContentsMargins(0, 0, 0, 0);
      auto* add = new QPushButton(tr("Add edges"));
      auto* remove = new QPushButton(tr("Remove"));
      add->setObjectName(QStringLiteral("add_edges"));
      h->addWidget(add);
      h->addWidget(remove);
      // Picks edges in the view, each added to the list.
      QPushButton* pick = pick_button(PickKind::Edge, tr("Edges"), true, [this](const PickResolution& r) {
        if (!r.edge) return false;
        auto& ww = *w_;
        const auto same = [&](const EdgeRef& e) {
          return (e.face == r.edge->face && e.other == r.edge->other) ||
                 (e.other && *e.other == r.edge->face && e.face == r.edge->other);
        };
        if (std::none_of(ww.edges.begin(), ww.edges.end(), same)) ww.edges.push_back(*r.edge);
        show_edges();
        emit changed();
        return true;
      });
      pick->setText(tr("Pick edges in view"));
      h->addWidget(pick);
      h->addStretch(1);
      f->addRow(QString(), row);
      w.edge_list = new QListWidget;
      w.edge_list->setMinimumHeight(90);
      f->addRow(tr("Edges"), w.edge_list);
      connect(add, &QPushButton::clicked, this, [this] {
        auto& ww = *w_;
        const auto face = ww.edge_face.get();
        if (!face) return;
        ww.edges.push_back({*face, ww.edge_other.get()});
        show_edges();
        emit changed();
      });
      connect(remove, &QPushButton::clicked, this, [this] {
        auto& ww = *w_;
        const int row_index = ww.edge_list->currentRow();
        if (row_index < 0 || row_index >= static_cast<int>(ww.edges.size())) return;
        ww.edges.erase(ww.edges.begin() + row_index);
        show_edges();
        emit changed();
      });
      w.p1 = param(original_.kind() == FeatureKind::Fillet ? tr("Radius") : tr("Distance"), ParamUnit::Length);
      break;
    }
    case FeatureKind::LinearPattern:
      w.features = CheckList<Id>(list(tr("Features to copy")), choices_.solid_features);
      w.axis = RefPicker<AxisRef>(combo(tr("Direction")), choices_.axes);
      w.p1 = param(tr("Count (with the original)"), ParamUnit::Count);
      w.p2 = param(tr("Spacing"), ParamUnit::Length);
      w.flag = check(tr("Reverse direction"));
      break;
    case FeatureKind::CircularPattern:
      w.features = CheckList<Id>(list(tr("Features to copy")), choices_.solid_features);
      w.axis = RefPicker<AxisRef>(combo(tr("Axis")), choices_.axes);
      w.p1 = param(tr("Count (with the original)"), ParamUnit::Count);
      w.angle = param(tr("Angle"), ParamUnit::Angle);
      w.flag = check(tr("Reverse direction"));
      break;
  }
  if (w.extent != nullptr) {
    auto update = [this] {
      const auto e = enum_of<Extent>(w_->extent);
      w_->p1->setEnabled(e == Extent::Blind || e == Extent::Symmetric);
      w_->plane.box()->setEnabled(e == Extent::UpToPlane);
    };
    connect(w.extent, &QComboBox::currentIndexChanged, this, update);
  }
  if (!choices_.variables.empty()) {
    QString names;
    for (const auto& v : choices_.variables) names += (names.isEmpty() ? "" : ", ") + qstr(v);
    auto* hint = new QLabel(tr("Expressions can use: %1").arg(names));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #666;"));
    f->addRow(hint);
  }
}

void FeatureDialog::set_feature(const Feature& feat) {
  auto& w = *w_;
  const QSignalBlocker block(this);
  w.name->setText(qstr(feat.name));
  std::visit(
      [&](const auto& d) {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, SketchFeature>) {
          w.plane.set(d.plane, !is_new_);
        } else if constexpr (std::is_same_v<T, DatumPlaneFeature>) {
          w.kind_of_definition->setCurrentIndex(static_cast<int>(d.definition.index()));
          if (const auto* o = std::get_if<PlaneOffset>(&d.definition)) {
            w.plane.set(o->base, !is_new_);
            w.p1->set_param(o->distance);
          } else if (const auto* t = std::get_if<PlaneThroughPoints>(&d.definition)) {
            w.point_a.set(t->a, !is_new_);
            w.point_b.set(t->b, !is_new_);
            w.point_c.set(t->c, !is_new_);
          } else if (const auto* a = std::get_if<PlaneAxisAngle>(&d.definition)) {
            w.page_axis.set(a->axis, !is_new_);
            w.page_plane_a.set(a->base, !is_new_);
            w.page_angle->set_param(a->angle);
          }
        } else if constexpr (std::is_same_v<T, DatumAxisFeature>) {
          w.kind_of_definition->setCurrentIndex(static_cast<int>(d.definition.index()));
          if (const auto* t = std::get_if<AxisThroughPoints>(&d.definition)) {
            w.point_a.set(t->a, !is_new_);
            w.point_b.set(t->b, !is_new_);
          } else if (const auto* i = std::get_if<AxisPlaneIntersection>(&d.definition)) {
            w.page_plane_a.set(i->a, !is_new_);
            w.page_plane_b.set(i->b, !is_new_);
          } else if (const auto* a = std::get_if<AxisAlong>(&d.definition)) {
            w.page_axis.set(a->axis, !is_new_);
          }
        } else if constexpr (std::is_same_v<T, DatumPointFeature>) {
          w.kind_of_definition->setCurrentIndex(static_cast<int>(d.definition.index()));
          if (const auto* c = std::get_if<PointAt>(&d.definition)) {
            w.p1->set_param(c->x);
            w.p2->set_param(c->y);
            w.p3->set_param(c->z);
          } else if (const auto* r = std::get_if<PointAtRef>(&d.definition)) {
            w.page_point.set(r->point, !is_new_);
          } else if (const auto* m = std::get_if<PointAxisPlane>(&d.definition)) {
            w.page_axis.set(m->axis, !is_new_);
            w.page_plane_a.set(m->plane, !is_new_);
          }
        } else if constexpr (std::is_same_v<T, ExtrudeFeature>) {
          if (!d.sketch.is_null()) w.sketch.set(d.sketch, !is_new_);
          set_enum(w.operation, d.operation);
          set_enum(w.extent, d.extent);
          w.p1->set_param(d.depth);
          w.plane.set(d.up_to, !is_new_);
          w.p2->set_param(d.draft);
          w.flag->setChecked(d.reverse);
          w.p1->setEnabled(d.extent == Extent::Blind || d.extent == Extent::Symmetric);
          w.plane.box()->setEnabled(d.extent == Extent::UpToPlane);
        } else if constexpr (std::is_same_v<T, RevolveFeature>) {
          if (!d.sketch.is_null()) w.sketch.set(d.sketch, !is_new_);
          set_enum(w.operation, d.operation);
          w.axis.set(d.axis, !is_new_);
          w.angle->set_param(d.angle);
          w.flag->setChecked(d.reverse);
        } else if constexpr (std::is_same_v<T, SweepFeature>) {
          if (!d.profile.is_null()) w.sketch.set(d.profile, !is_new_);
          if (!d.path.is_null()) w.sketch2.set(d.path, !is_new_);
          set_enum(w.operation, d.operation);
          set_enum(w.orientation, d.orientation);
        } else if constexpr (std::is_same_v<T, LoftFeature>) {
          w.sketches.set(d.sections);
          set_enum(w.operation, d.operation);
          w.flag->setChecked(d.ruled);
        } else if constexpr (std::is_same_v<T, ShellFeature>) {
          w.faces.set(d.open);
          w.p1->set_param(d.thickness);
          w.flag->setChecked(d.outward);
        } else if constexpr (std::is_same_v<T, FilletFeature> || std::is_same_v<T, ChamferFeature>) {
          w.edges = d.edges;
          show_edges();
          if constexpr (std::is_same_v<T, FilletFeature>) {
            w.p1->set_param(d.radius);
          } else {
            w.p1->set_param(d.distance);
          }
        } else if constexpr (std::is_same_v<T, LinearPatternFeature>) {
          w.features.set(d.features);
          w.axis.set(d.direction, !is_new_);
          w.p1->set_param(d.count);
          w.p2->set_param(d.spacing);
          w.flag->setChecked(d.reverse);
        } else if constexpr (std::is_same_v<T, CircularPatternFeature>) {
          w.features.set(d.features);
          w.axis.set(d.axis, !is_new_);
          w.p1->set_param(d.count);
          w.angle->set_param(d.angle);
          w.flag->setChecked(d.reverse);
        }
      },
      feat.definition);
  if (w.pages != nullptr) w.pages->setCurrentIndex(w.kind_of_definition->currentIndex());
}

void FeatureDialog::show_edges() {
  auto& w = *w_;
  if (w.edge_list == nullptr) return;
  const QSignalBlocker block(w.edge_list);
  w.edge_list->clear();
  for (const auto& e : w.edges) {
    QString text = qstr(choices_.label_of(e.face));
    if (text.isEmpty()) text = tr("(missing face)");
    text += e.other ? QStringLiteral(" / ") + qstr(choices_.label_of(*e.other)) : tr(" (every edge)");
    w.edge_list->addItem(text);
  }
}

Feature FeatureDialog::feature() const {
  const auto& w = *w_;
  Feature out = original_;
  out.name = sstr(w.name->text().trimmed());
  auto p = [](const ParamEdit* e) { return e->param().value_or(feature::Param{}); };
  auto id_or_null = [](const RefPicker<Id>& r) { return r.get().value_or(Id{}); };
  std::visit(
      [&](auto& d) {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, SketchFeature>) {
          if (const auto r = w.plane.get()) d.plane = *r;
        } else if constexpr (std::is_same_v<T, DatumPlaneFeature>) {
          switch (w.kind_of_definition->currentIndex()) {
            case 0: d.definition = PlaneOffset{w.plane.get().value_or(PrincipalPlane::XY), p(w.p1)}; break;
            case 1:
              if (w.point_a.get() && w.point_b.get() && w.point_c.get()) {
                d.definition = PlaneThroughPoints{*w.point_a.get(), *w.point_b.get(), *w.point_c.get()};
              }
              break;
            default:
              d.definition = PlaneAxisAngle{w.page_axis.get().value_or(PrincipalAxis::X),
                                            w.page_plane_a.get().value_or(PrincipalPlane::XY), p(w.page_angle)};
          }
        } else if constexpr (std::is_same_v<T, DatumAxisFeature>) {
          switch (w.kind_of_definition->currentIndex()) {
            case 0:
              if (w.point_a.get() && w.point_b.get()) d.definition = AxisThroughPoints{*w.point_a.get(), *w.point_b.get()};
              break;
            case 1:
              d.definition = AxisPlaneIntersection{w.page_plane_a.get().value_or(PrincipalPlane::XY),
                                                   w.page_plane_b.get().value_or(PrincipalPlane::YZ)};
              break;
            default: d.definition = AxisAlong{w.page_axis.get().value_or(PrincipalAxis::Z)};
          }
        } else if constexpr (std::is_same_v<T, DatumPointFeature>) {
          switch (w.kind_of_definition->currentIndex()) {
            case 0: d.definition = PointAt{p(w.p1), p(w.p2), p(w.p3)}; break;
            case 1:
              if (w.page_point.get()) d.definition = PointAtRef{*w.page_point.get()};
              break;
            default:
              d.definition = PointAxisPlane{w.page_axis.get().value_or(PrincipalAxis::Z),
                                            w.page_plane_a.get().value_or(PrincipalPlane::XY)};
          }
        } else if constexpr (std::is_same_v<T, ExtrudeFeature>) {
          d.sketch = id_or_null(w.sketch);
          d.operation = enum_of<Operation>(w.operation);
          d.extent = enum_of<Extent>(w.extent);
          d.depth = p(w.p1);
          if (const auto r = w.plane.get()) d.up_to = *r;
          d.draft = p(w.p2);
          d.reverse = w.flag->isChecked();
        } else if constexpr (std::is_same_v<T, RevolveFeature>) {
          d.sketch = id_or_null(w.sketch);
          d.operation = enum_of<Operation>(w.operation);
          if (const auto r = w.axis.get()) d.axis = *r;
          d.angle = p(w.angle);
          d.reverse = w.flag->isChecked();
        } else if constexpr (std::is_same_v<T, SweepFeature>) {
          d.profile = id_or_null(w.sketch);
          d.path = id_or_null(w.sketch2);
          d.operation = enum_of<Operation>(w.operation);
          d.orientation = enum_of<SweepOrientation>(w.orientation);
        } else if constexpr (std::is_same_v<T, LoftFeature>) {
          d.sections = w.sketches.get();
          d.operation = enum_of<Operation>(w.operation);
          d.ruled = w.flag->isChecked();
        } else if constexpr (std::is_same_v<T, ShellFeature>) {
          d.open = w.faces.get();
          d.thickness = p(w.p1);
          d.outward = w.flag->isChecked();
        } else if constexpr (std::is_same_v<T, FilletFeature>) {
          d.edges = w.edges;
          d.radius = p(w.p1);
        } else if constexpr (std::is_same_v<T, ChamferFeature>) {
          d.edges = w.edges;
          d.distance = p(w.p1);
        } else if constexpr (std::is_same_v<T, LinearPatternFeature>) {
          d.features = w.features.get();
          if (const auto r = w.axis.get()) d.direction = *r;
          d.count = p(w.p1);
          d.spacing = p(w.p2);
          d.reverse = w.flag->isChecked();
        } else if constexpr (std::is_same_v<T, CircularPatternFeature>) {
          d.features = w.features.get();
          if (const auto r = w.axis.get()) d.axis = *r;
          d.count = p(w.p1);
          d.angle = p(w.angle);
          d.reverse = w.flag->isChecked();
        }
      },
      out.definition);
  return out;
}

// --- pick in view (M16) --------------------------------------------------------------------

QPushButton* FeatureDialog::pick_button(PickKind kind, const QString& label, bool multiple,
                                        std::function<bool(const PickResolution&)> apply) {
  auto* b = new QPushButton(tr("Pick"));
  b->setCheckable(true);
  b->setToolTip(kind == PickKind::Edge ? tr("Pick edges in the viewport") : tr("Pick a face in the viewport"));
  const int index = static_cast<int>(fields_.size());
  b->setObjectName(QStringLiteral("pick_%1").arg(index));
  fields_.push_back({kind, label, multiple, std::move(apply), b});
  connect(b, &QPushButton::clicked, this, [this, index](bool on) { begin_pick(on ? index : -1); });
  return b;
}

void FeatureDialog::begin_pick(int field) {
  if (field >= pick_field_count()) field = -1;
  if (field == picking_) return;
  picking_ = field;
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    const QSignalBlocker block(fields_[i].button);
    fields_[i].button->setChecked(static_cast<int>(i) == field);
  }
  if (field >= 0) {
    const auto k = fields_[static_cast<std::size_t>(field)].kind;
    set_message(k == PickKind::Edge ? tr("Click edges in the viewport; press Pick again when done.")
                                    : tr("Click a face in the viewport."),
                false);
  } else {
    set_message({});
  }
  emit pickRequested(field);
}

bool FeatureDialog::apply_pick(const PickResolution& pick) {
  if (picking_ < 0) return false;
  const PickField& f = fields_[static_cast<std::size_t>(picking_)];
  if (!pick.ok()) {
    set_message(QString::fromStdString(pick.why));
    return false;
  }
  if (!f.apply(pick)) {
    set_message(f.kind == PickKind::Plane ? tr("%1 isn't a plane; pick a flat face.").arg(qstr(pick.label))
                                          : tr("That pick doesn't fit %1.").arg(f.label));
    return false;
  }
  set_message(tr("Picked %1").arg(qstr(pick.label)), false);
  if (!f.multiple) {
    const QString keep = message();
    end_pick();
    set_message(keep, false);
  }
  return true;
}

bool FeatureDialog::complete() const {
  const auto& w = *w_;
  // A datum checks only the page of its definition: the other pages'
  // fields (empty for a datum defined by points, say) aren't used (H7).
  // Every other feature's numbers are all on show.
  if (w.pages == nullptr) {
    auto ok = [](const ParamEdit* e) { return e == nullptr || !e->isEnabled() || e->param().has_value(); };
    if (!(ok(w.p1) && ok(w.p2) && ok(w.p3) && ok(w.angle))) return false;
  }
  switch (original_.kind()) {
    case FeatureKind::Sketch: return w.plane.get().has_value();
    case FeatureKind::DatumPlane:
      switch (w.kind_of_definition->currentIndex()) {
        case 0: return w.plane.get().has_value() && w.p1->param().has_value();
        case 1: return w.point_a.get() && w.point_b.get() && w.point_c.get();
        default: return w.page_axis.get() && w.page_plane_a.get() && w.page_angle->param();
      }
    case FeatureKind::DatumAxis:
      switch (w.kind_of_definition->currentIndex()) {
        case 0: return w.point_a.get() && w.point_b.get();
        case 1: return w.page_plane_a.get() && w.page_plane_b.get();
        default: return w.page_axis.get().has_value();
      }
    case FeatureKind::DatumPoint:
      switch (w.kind_of_definition->currentIndex()) {
        case 0: return w.p1->param() && w.p2->param() && w.p3->param();
        case 1: return w.page_point.get().has_value();
        default: return w.page_axis.get() && w.page_plane_a.get();
      }
    case FeatureKind::Extrude:
      return w.sketch.get() && (enum_of<Extent>(w.extent) != Extent::UpToPlane || w.plane.get());
    case FeatureKind::Revolve: return w.sketch.get() && w.axis.get();
    case FeatureKind::Sweep: return w.sketch.get() && w.sketch2.get();
    case FeatureKind::Loft: return w.sketches.get().size() >= 2;
    case FeatureKind::Shell: return true;
    case FeatureKind::Fillet:
    case FeatureKind::Chamfer: return !w.edges.empty();
    case FeatureKind::LinearPattern:
    case FeatureKind::CircularPattern: return !w.features.get().empty() && w.axis.get();
  }
  return true;
}

}  // namespace vs::app

// M16 in the main window, on a real display and GPU: dragging a layout
// dimension with previews (above 30 fps), picking every reference a feature
// dialog takes on the bracket by clicking in the viewport, stale contents
// with progress and cancel, swapping contents, and habit guidance.

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>

#include "authoring.hpp"
#include "feature_dialog.hpp"
#include "main_window.hpp"
#include "model_dialogs.hpp"
#include "picking.hpp"
#include "reference_choices.hpp"
#include "samples.hpp"
#include "sketch_mode.hpp"
#include "viewport.hpp"
#include "vs/kernel/queries.hpp"
#include "vs/model/build.hpp"
#include "wait.hpp"

using namespace vs;
using core::Id;
using Catch::Matchers::WithinAbs;

namespace {

void settle(app::MainWindow& w) {
  REQUIRE(samples::wait_until(
      [&] {
        const auto s = w.session().latest();
        return !w.session().busy() && s && !s->preview && s->project == w.session().project_ptr();
      },
      180000));
  QApplication::processEvents();
}

core::fvec3 operator_sub(core::fvec3 a, core::fvec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

core::dvec3 to64(core::fvec3 v) { return {v.x, v.y, v.z}; }

void send_mouse(QWidget& w, QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons) {
  QMouseEvent e(type, at, w.mapToGlobal(at), button, buttons, Qt::NoModifier);
  QApplication::sendEvent(&w, &e);
}

const feature::Feature* find_feature(const feature::FeatureTree& tree, const std::string& label) {
  for (const auto& f : tree.features()) {
    if (tree.label(f.id) == label) return &f;
  }
  return nullptr;
}

// The triangles of face `f` of `item`, largest first (a few).
std::vector<std::uint32_t> big_triangles(const app::DisplayMesh& m, const app::DisplayMesh::FacePart& f) {
  std::vector<std::pair<double, std::uint32_t>> by_area;
  for (std::uint32_t t = f.first; t < f.first + f.count; ++t) {
    const auto& a = m.positions[m.indices[3 * t]];
    const auto& b = m.positions[m.indices[3 * t + 1]];
    const auto& c = m.positions[m.indices[3 * t + 2]];
    const core::dvec3 n = cross(to64(operator_sub(b, a)), to64(operator_sub(c, a)));
    by_area.emplace_back(length(n), t);
  }
  std::sort(by_area.begin(), by_area.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
  std::vector<std::uint32_t> out;
  for (std::size_t i = 0; i < by_area.size() && i < 6; ++i) out.push_back(by_area[i].second);
  return out;
}

// Clicks at `at` until the viewport picks `want` (clicking again at one
// place picks what lies behind); true when it did.
bool click_until(app::MainWindow& w, app::FeatureDialog& d, int field, QPointF at, const app::PickedPart& want) {
  std::string seen;
  for (int k = 0; k < 10; ++k) {
    d.begin_pick(field);
    const auto got = w.viewport().pick_at(at);
    if (!got) break;
    seen += " " + std::string(app::to_string(got->role)) + "#" + std::to_string(got->index);
    if (*got == want) return true;
  }
  UNSCOPED_INFO("clicks at (" << at.x() << ", " << at.y() << ") picked" << seen);
  return false;
}

// Picks face `face` of `item` in the viewport: looks at it from outside,
// clicks one of its biggest triangles. True once a click picked it.
bool click_face(app::MainWindow& w, app::FeatureDialog& d, int field, const app::SceneItem& item, std::size_t face) {
  const auto& m = item.mesh;
  const app::PickedPart want{item.subject, item.role, app::PickedPart::Kind::Face, face};
  for (const auto t : big_triangles(m, m.faces[face])) {
    const auto& a = m.positions[m.indices[3 * t]];
    const auto& b = m.positions[m.indices[3 * t + 1]];
    const auto& c = m.positions[m.indices[3 * t + 2]];
    const core::dvec3 n = normalize(cross(to64(operator_sub(b, a)), to64(operator_sub(c, a))));
    w.viewport().look_along(n + core::dvec3{0.02, 0.03, 0.01});  // not quite square on
    QApplication::processEvents();
    const core::dvec3 centre{(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0, (a.z + b.z + c.z) / 3.0};
    const auto at = w.viewport().project(centre);
    if (!at) continue;
    if (click_until(w, d, field, *at, want)) return true;
  }
  return false;
}

// Picks edge `edge` of `item`: looks at it between its faces, clicks its
// middle.
bool click_edge(app::MainWindow& w, app::FeatureDialog& d, int field, const app::SceneItem& item, std::size_t edge) {
  const auto& m = item.mesh;
  const auto [fa, fb] = m.edge_faces[edge];
  const auto normal_of = [&](int f) {
    const auto& part = m.faces[static_cast<std::size_t>(f)];
    core::dvec3 sum{};
    for (std::uint32_t t = part.first; t < part.first + part.count; ++t) {
      const auto& a = m.positions[m.indices[3 * t]];
      const auto& b = m.positions[m.indices[3 * t + 1]];
      const auto& c = m.positions[m.indices[3 * t + 2]];
      sum = sum + cross(to64(operator_sub(b, a)), to64(operator_sub(c, a)));
    }
    return normalize(sum);
  };
  const app::PickedPart want{item.subject, item.role, app::PickedPart::Kind::Edge, edge};
  const auto& line = m.edges[edge];
  for (const double tilt : {0.0, 0.3, -0.3}) {
    const core::dvec3 na = normal_of(fa), nb = normal_of(fb);
    core::dvec3 eye = normalize(na + nb) + (na - nb) * tilt;
    if (length(eye) < 1e-6) eye = na;
    w.viewport().look_along(eye + core::dvec3{0.01, 0.02, 0.015});
    QApplication::processEvents();
    for (const double frac : {0.5, 0.3, 0.7}) {
      const auto& p = line[std::min(line.size() - 1, static_cast<std::size_t>(frac * (line.size() - 1)))];
      core::dvec3 q = to64(p);
      if (line.size() == 2) q = to64(line[0]) + (to64(line[1]) - to64(line[0])) * frac;
      const auto at = w.viewport().project(q);
      if (at && click_until(w, d, field, *at, want)) return true;
    }
  }
  return false;
}

// Every face choice of `d` can be picked (as `field` takes it) by clicking
// a face that lies on it, and resolves to the first choice on its surface:
// what the list gives for that surface.
void pick_every_face(app::MainWindow& w, app::FeatureDialog& d, int field, const Id& tree, bool planes) {
  const auto s = w.session().latest();
  REQUIRE(s);
  const auto& ev = s->evaluation;
  const auto pickable = app::pickable_items(ev, tree);
  const auto& choices = d.choices();
  std::size_t picked = 0;
  for (const auto& choice : choices.faces) {
    if (planes) {
      // Planar faces are planes; a patch is offered as its surface.
      const feature::PlaneRef as_plane = choice.ref.name == "patch"
                                             ? feature::PlaneRef{feature::FeatureRef{choice.ref.feature}}
                                             : feature::PlaneRef{choice.ref};
      if (std::none_of(choices.planes.begin(), choices.planes.end(),
                       [&](const auto& c) { return c.ref == as_plane; })) {
        continue;  // cylindrical: not a plane
      }
    }
    INFO(choice.label);
    const auto target = app::named_surface(ev, tree, choice.ref);
    REQUIRE(target);
    bool done = false;
    for (const auto& [subject, role] : pickable) {
      const auto* item = s->scene.find(subject, role);
      if (item == nullptr || done) continue;
      for (std::size_t f = 0; f < item->mesh.faces.size() && !done; ++f) {
        if (role != app::ItemRole::ConnectionSurface) {
          const auto surface = app::face_surface(item->mesh.faces[f].face);
          if (!surface || !app::same_surface(*surface, *target)) continue;
        } else if (choice.ref != feature::FaceRef{subject, "patch"}) {
          continue;
        }
        if (!click_face(w, d, field, *item, f)) continue;
        REQUIRE(w.last_pick().has_value());
        const auto& pick = *w.last_pick();
        REQUIRE(pick.face.has_value());
        // The reference a pick gives lies on the chosen face's surface, and
        // is the first such choice in the list.
        const auto got = app::named_surface(ev, tree, *pick.face);
        REQUIRE(got);
        CHECK(app::same_surface(*got, *target));
        if (planes) {
          REQUIRE(pick.plane.has_value());
          const auto plane = d.feature();
          if (const auto* dp = plane.as<feature::DatumPlaneFeature>()) {
            CHECK(std::get<feature::PlaneOffset>(dp->definition).base == *pick.plane);
          }
        }
        done = true;
      }
    }
    CHECK(done);
    picked += done ? 1 : 0;
  }
  CHECK(picked > 0);
  d.end_pick();
}

// Every edge of the pickable items between two named faces can be picked
// and lands in the fillet's edge list as the pair of list choices.
void pick_every_edge(app::MainWindow& w, app::FeatureDialog& d, int field, const Id& tree) {
  const auto s = w.session().latest();
  REQUIRE(s);
  const auto& ev = s->evaluation;
  std::size_t picked = 0;
  for (const auto& [subject, role] : app::pickable_items(ev, tree)) {
    const auto* item = s->scene.find(subject, role);
    if (item == nullptr || role == app::ItemRole::ConnectionSurface) continue;
    for (std::size_t e = 0; e < item->mesh.edges.size(); ++e) {
      const auto expected = app::resolve_pick(s->scene, ev, tree, d.choices(),
                                              {subject, role, app::PickedPart::Kind::Edge, e});
      if (!expected.edge) continue;
      INFO(expected.label);
      const bool ok = click_edge(w, d, field, *item, e);
      CHECK(ok);
      if (!ok) continue;
      REQUIRE(w.last_pick().has_value());
      CHECK(w.last_pick()->edge == expected.edge);
      const auto f = d.feature();
      const auto* fillet = f.as<feature::FilletFeature>();
      REQUIRE(fillet != nullptr);
      CHECK(std::any_of(fillet->edges.begin(), fillet->edges.end(), [&](const feature::EdgeRef& r) {
        return (r.face == expected.edge->face && r.other == expected.edge->other) ||
               (r.other && *r.other == expected.edge->face && r.face == expected.edge->other);
      }));
      ++picked;
    }
  }
  CHECK(picked > 0);
  d.end_pick();
}

}  // namespace

TEST_CASE("Every reference a feature dialog takes on the bracket can be picked in the viewport",
          "[app][gui][picking][acceptance]") {
  app::MainWindow w;
  w.resize(1280, 860);
  w.show();
  REQUIRE(w.viewport().has_gpu());
  REQUIRE(w.open_project(samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj"));
  settle(w);
  const Id base = w.history().project().document.find_volume("Base")->id;

  SECTION("in the layout") {
    auto* datum = w.insert_feature(feature::FeatureKind::DatumPlane);
    REQUIRE(datum != nullptr);
    REQUIRE(datum->pick_field_count() >= 1);
    CHECK(datum->pick_kind(0) == app::FeatureDialog::PickKind::Plane);
    pick_every_face(w, *datum, 0, {}, true);
    datum->reject();
    settle(w);

    auto* fillet = w.insert_feature(feature::FeatureKind::Fillet);
    REQUIRE(fillet != nullptr);
    REQUIRE(fillet->pick_field_count() == 3);
    CHECK(fillet->pick_kind(0) == app::FeatureDialog::PickKind::Face);
    CHECK(fillet->pick_kind(2) == app::FeatureDialog::PickKind::Edge);
    pick_every_face(w, *fillet, 0, {}, false);
    pick_every_edge(w, *fillet, 2, {});
    fillet->reject();
  }
  SECTION("in context") {
    REQUIRE(w.edit_in_context(base));
    settle(w);
    auto* datum = w.insert_feature(feature::FeatureKind::DatumPlane);
    REQUIRE(datum != nullptr);
    pick_every_face(w, *datum, 0, base, true);
    datum->reject();
    settle(w);
    auto* fillet = w.insert_feature(feature::FeatureKind::Fillet);
    REQUIRE(fillet != nullptr);
    pick_every_face(w, *fillet, 0, base, false);
    pick_every_edge(w, *fillet, 2, base);
    // Nothing of another volume can be picked: the upright isn't on the list.
    const auto upright = w.history().project().document.find_volume("Upright")->id;
    const auto items = app::pickable_items(w.session().latest()->evaluation, base);
    CHECK(std::none_of(items.begin(), items.end(), [&](const auto& p) { return p.first == upright; }));
    fillet->reject();
  }
  CHECK(w.viewport().pick_mode() == app::PickMode::None);
}

TEST_CASE("Dragging a layout dimension on the bracket keeps the viewport above 30 fps with previews",
          "[app][gui][drag][acceptance]") {
  app::MainWindow w;
  w.resize(1280, 800);
  w.show();
  REQUIRE(w.viewport().has_gpu());
  REQUIRE(w.open_project(samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj"));
  settle(w);
  samples::wait_until([] { return false; }, 500);  // the viewer's first frames

  const auto& doc = w.history().project().document;
  const Id base = doc.find_volume("Base")->id;
  const auto* block = find_feature(doc.layout, "Base block");
  REQUIRE(block != nullptr);
  w.select(app::Selection::of_feature(block->id));
  REQUIRE(w.dimension_handles().size() == 3);
  CHECK(w.viewport().handle_count() == 3);
  const auto depth = *std::find_if(w.dimension_handles().begin(), w.dimension_handles().end(),
                                   [](const auto& h) { return h.label.find("depth") != std::string::npos; });

  std::size_t frames = 0, previews = 0, boundary_only = 0;
  double max_frame = 0;
  QObject::connect(&w.viewport(), &app::Viewport::frameRendered, [&](double ms) {
    ++frames;
    max_frame = std::max(max_frame, ms);
  });
  QObject::connect(&w, &app::MainWindow::dragPreviewShown, [&] {
    ++previews;
    const auto s = w.session().latest();
    // The base is changing: its boundary only, no stale contents.
    if (s && s->drag && s->scene.find(base, app::ItemRole::Contents) == nullptr &&
        s->scene.find(base, app::ItemRole::Boundary) != nullptr) {
      ++boundary_only;
    }
  });

  REQUIRE(w.begin_drag(depth));
  const double seconds = 3.0;
  QElapsedTimer clock;
  QTimer ticker;
  ticker.setTimerType(Qt::PreciseTimer);
  ticker.setInterval(16);  // a mouse moving every frame
  QObject::connect(&ticker, &QTimer::timeout, [&] {
    const double t = static_cast<double>(clock.elapsed()) / 1000.0 / seconds;
    w.drag_to(10.0 + 8.0 * std::min(1.0, t));
    w.viewport().update();
  });
  clock.start();
  frames = 0;
  ticker.start();
  samples::wait_until([&] { return clock.elapsed() > static_cast<qint64>(seconds * 1000); }, 60000);
  ticker.stop();
  const double elapsed = static_cast<double>(clock.elapsed()) / 1000.0;
  const double fps = static_cast<double>(frames) / elapsed;
  WARN("drag: " << frames << " frames in " << elapsed << " s (" << fps << " fps), longest frame " << max_frame
                << " ms, " << previews << " previews");
  CHECK(fps > 30.0);
  CHECK(max_frame < 100.0);
  CHECK(previews >= 3);
  CHECK(boundary_only >= 1);
  CHECK(w.history().undo_count() == 0);  // nothing kept while dragging

  w.drag_to(18.0);
  REQUIRE_FALSE(w.end_drag(true).has_errors());
  settle(w);
  CHECK(w.history().undo_count() == 1);
  CHECK(w.history().undo_label() == "Drag " + depth.label);
  CHECK(w.history().project().document.layout.variables().find("base_h")->expression == "18 mm");
  const auto s = w.session().latest();
  const auto* vb = s->evaluation.build.find_volume(base);
  REQUIRE(vb != nullptr);
  CHECK_THAT(kernel::bounding_box(vb->boundary).max.z, WithinAbs(18, 1e-6));
  // The full generation brought the base's contents back.
  CHECK(s->scene.find(base, app::ItemRole::Contents) != nullptr);
  w.undo();
  settle(w);
  CHECK(w.history().project().document.layout.variables().find("base_h")->expression == "10 mm");
}

TEST_CASE("A handle dragged with the mouse changes its dimension in one step", "[app][gui][drag]") {
  app::MainWindow w;
  w.resize(1280, 800);
  w.show();
  REQUIRE(w.open_project(samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj"));
  settle(w);
  const auto& doc = w.history().project().document;
  const auto* block = find_feature(doc.layout, "Base block");
  REQUIRE(block != nullptr);
  w.viewport().set_view(app::StandardView::Front);
  w.select(app::Selection::of_feature(block->id));
  const auto& handles = w.dimension_handles();
  const auto it = std::find_if(handles.begin(), handles.end(),
                               [](const auto& h) { return h.label.find("depth") != std::string::npos; });
  REQUIRE(it != handles.end());
  const auto from = w.viewport().project(it->anchor);
  const auto up = w.viewport().project(it->anchor + it->direction * 5.0);
  REQUIRE(from);
  REQUIRE(up);
  REQUIRE(w.viewport().handle_at(*from).has_value());
  auto& vp = w.viewport();
  send_mouse(vp, QEvent::MouseButtonPress, *from, Qt::LeftButton, Qt::LeftButton);
  CHECK(w.dragging());
  const QPointF mid = *from + (*up - *from) * 0.5;
  send_mouse(vp, QEvent::MouseMove, mid, Qt::NoButton, Qt::LeftButton);
  send_mouse(vp, QEvent::MouseMove, *up, Qt::NoButton, Qt::LeftButton);
  send_mouse(vp, QEvent::MouseButtonRelease, *up, Qt::LeftButton, Qt::NoButton);
  CHECK_FALSE(w.dragging());
  settle(w);
  REQUIRE(w.history().undo_count() == 1);
  const auto* base_h = w.history().project().document.layout.variables().find("base_h");
  const double v = std::stod(base_h->expression);
  CHECK_THAT(v, WithinAbs(15.0, 0.5));  // 5 mm up the handle, to a pixel
}

TEST_CASE("Sketch mode drags a dimension by its label", "[app][gui][drag]") {
  app::MainWindow w;
  w.show();
  w.new_project("Drag");
  settle(w);
  app::SketchMode* mode = w.new_sketch(feature::PrincipalPlane::XY, "Bar");
  REQUIRE(mode != nullptr);
  auto& ed = mode->editor();
  sketch::Sketch sk = ed.sketch();
  // Through the editor, as the tools would: a fixed horizontal bar 30 long.
  mode->set_tool(app::SketchTool::Line);
  ed.click({0, 0});
  ed.click({30, 0});
  ed.finish_tool();
  mode->set_tool(app::SketchTool::Select);
  const Id start = ed.nearest_point({0, 0}, 0.01);
  const Id bar = ed.nearest_entity({15, 0}, 0.5);
  mode->view().set_selection({start});
  REQUIRE(mode->constrain(sketch::ConstraintKind::Fix));
  mode->view().set_selection({bar});
  REQUIRE(mode->constrain(sketch::ConstraintKind::Distance));
  const Id dim = mode->view().selection().front();
  REQUIRE(mode->set_dimension_text(dim, QStringLiteral("30")));
  mode->view().fit();
  mode->view().repaint();
  const QRectF label = mode->view().dimension_label(dim);
  REQUIRE_FALSE(label.isEmpty());
  const auto steps = ed.can_undo();
  (void)steps;
  // 10 mm to the right, in screen pixels.
  const double px = mode->view().to_screen({10, 0}).x() - mode->view().to_screen({0, 0}).x();
  auto& view = mode->view();
  send_mouse(view, QEvent::MouseButtonPress, label.center(), Qt::LeftButton, Qt::LeftButton);
  send_mouse(view, QEvent::MouseMove, label.center() + QPointF(px / 2, 0), Qt::NoButton, Qt::LeftButton);
  send_mouse(view, QEvent::MouseMove, label.center() + QPointF(px, 0), Qt::NoButton, Qt::LeftButton);
  send_mouse(view, QEvent::MouseButtonRelease, label.center() + QPointF(px, 0), Qt::LeftButton, Qt::NoButton);
  CHECK_THAT(ed.sketch().find_constraint(dim)->dimension->value, WithinAbs(40, 0.1));
  const auto end = ed.nearest_point({40, 0}, 0.2);
  CHECK_FALSE(end.is_null());  // solved: the bar is 40 long
  ed.undo();  // one drag, one step
  CHECK_THAT(ed.sketch().find_constraint(dim)->dimension->value, WithinAbs(30, 1e-9));
  mode->finish(false);
}

TEST_CASE("Stale contents stay grey until replaced, with progress and cancel", "[app][gui][acceptance]") {
  app::MainWindow w;
  w.resize(1280, 800);
  w.show();
  w.session().set_project(samples::long_procedural(0.3));
  settle(w);
  const Id v = w.history().project().document.volumes.at(0).id;
  REQUIRE(w.session().latest()->evaluation.find(v)->status == model::VolumeStatus::Current);

  // A new seed: the volume regenerates; meanwhile the old contents show grey.
  bool stale_shown = false, progress_shown = false;
  auto c = QObject::connect(&w.session(), &app::Session::evaluated, [&](app::SnapshotPtr s) {
    if (!s->preview) return;
    const auto* item = s->scene.find(v, app::ItemRole::Contents);
    stale_shown = item != nullptr && item->stale && item->color == app::kStaleColor;
  });
  model::Volume changed = *w.history().project().document.find_volume(v);
  changed.seed += 1;
  REQUIRE_FALSE(w.apply(app::update_volume(changed)).has_errors());
  REQUIRE(samples::wait_until([&] { return stale_shown; }, 30000));
  QObject::disconnect(c);
  progress_shown = w.progress_bar().isVisible();
  CHECK(progress_shown);
  CHECK(w.cancel_action().isEnabled());

  // Cancel: the evaluation stops, the old contents stay (grey), the bar goes.
  bool full = false;
  auto c2 = QObject::connect(&w, &app::MainWindow::evaluationShown, [&] { full = true; });
  w.cancel_action().trigger();
  REQUIRE(samples::wait_until([&] { return full; }, 60000));
  QObject::disconnect(c2);
  const auto s = w.session().latest();
  CHECK(s->evaluation.cancelled);
  const auto* item = s->scene.find(v, app::ItemRole::Contents);
  REQUIRE(item != nullptr);
  CHECK(item->stale);
  CHECK_FALSE(w.progress_bar().isVisible());
  CHECK(w.status_text().contains(QStringLiteral("cancelled")));
}

TEST_CASE("Swapping contents in the window touches nothing outside the volume", "[app][gui][swap][acceptance]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(samples::source_dir() / "libs" / "vs_model" / "tests" / "data" / "bracket.vsproj"));
  settle(w);
  const Id upright = w.history().project().document.find_volume("Upright")->id;
  const auto keys = [&] {
    std::map<Id, std::optional<core::Hash256>> out;
    for (const auto& r : w.session().latest()->evaluation.volumes) {
      if (r.volume != upright) out[r.volume] = r.key;
    }
    return out;
  };
  const auto before = keys();
  using K = app::ContentsKind;
  for (const K k : {K::Placeholder, K::Procedural, K::Baked, K::Procedural, K::Parametric}) {
    INFO(app::to_string(k));
    REQUIRE_FALSE(w.swap_contents(upright, k).has_errors());
    settle(w);
    CHECK(app::contents_kind(*w.history().project().document.find_volume(upright)) == k);
    CHECK(keys() == before);
  }
  CHECK(w.history().undo_count() == 5);
}

TEST_CASE("Fillet, cut or mate on generated contents shows guidance with the right action", "[app][gui][guidance]") {
  app::MainWindow w;
  w.show();
  REQUIRE(w.open_project(samples::source_dir() / "libs" / "vs_proc" / "tests" / "data" / "bolt_bosses.vsproj"));
  settle(w);
  const auto& doc = w.history().project().document;
  const Id bosses = doc.find_volume("Bosses")->id;

  // Insert > Fillet with the procedural volume selected: guidance, no dialog.
  w.select(app::Selection::of_volume(bosses));
  CHECK(w.insert_feature(feature::FeatureKind::Fillet) == nullptr);
  REQUIRE(w.shown_guidance().has_value());
  CHECK(w.shown_guidance()->habit == app::Habit::Fillet);
  w.act_on_guidance();
  CHECK(w.graph_volume() == bosses);  // the procedure is open
  CHECK_FALSE(w.shown_guidance().has_value());
  w.leave_graph();

  REQUIRE(w.guide(bosses, app::Habit::Cut));
  w.act_on_guidance();
  CHECK(qobject_cast<app::KeepOutDialog*>(w.open_dialog()) != nullptr);
  w.open_dialog()->reject();
  settle(w);

  REQUIRE(w.guide(bosses, app::Habit::Mate));
  w.act_on_guidance();
  CHECK(w.selection() == app::Selection::of_surface(doc.connections.at(0).id));

  w.guide(bosses, app::Habit::Cut);
  w.dismiss_guidance();
  CHECK_FALSE(w.shown_guidance().has_value());
}

TEST_CASE("A drag takes down the progress of the regeneration it supersedes", "[app][gui][drag]") {
  app::MainWindow w;
  w.resize(1280, 800);
  w.show();
  bool started = false;
  QObject::connect(&w.session(), &app::Session::progressed, [&](int, int) { started = true; });
  w.session().set_project(samples::long_procedural(0.15));
  REQUIRE(samples::wait_until([&] { return started; }, 60000));
  CHECK(w.progress_bar().isVisible());
  // The drag cancels the regeneration, whose full snapshot never comes:
  // its progress bar and Cancel go with it (H7).
  bool drag_shown = false;
  QObject::connect(&w, &app::MainWindow::dragPreviewShown, [&] { drag_shown = true; });
  w.session().drag_preview(w.session().project_ptr());
  REQUIRE(samples::wait_until([&] { return drag_shown; }, 60000));
  samples::wait_until([] { return false; }, 200);
  CHECK_FALSE(w.progress_bar().isVisible());
  CHECK_FALSE(w.cancel_action().isEnabled());
  // Releasing regenerates, with its own progress, and finishes.
  bool full = false;
  QObject::connect(&w, &app::MainWindow::evaluationShown, [&] { full = true; });
  w.session().regenerate();
  CHECK(w.progress_bar().isVisible());
  REQUIRE(samples::wait_until([&] { return full; }, 300000));
  CHECK_FALSE(w.progress_bar().isVisible());
}

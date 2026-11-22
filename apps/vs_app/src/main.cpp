// vs_app: the Volumetric CAD Studio desktop application (M13 to M15).
//
//   vs_app [project] [--select <volume or surface name>] [--view iso|top|front|right|...]
//          [--context <volume name>] [--sketch <sketch name>] [--edit <feature name>]
//          [--graph <volume name> [--node <node name>] [--slice]]
//          [--drawing <name>] [--drawing-image <out.bmp>]
//          [--size <w>x<h>] [--screenshot <out.bmp>] [--viewport-image <out.bmp>] [--cpu]
//
// (vcpkg's Qt here is built without PNG support, so images are BMP.)
//
// --context edits a volume's contents in context; --sketch opens sketch
// mode on a sketch of the tree being edited; --edit opens a feature's
// dialog (with --screenshot it is saved too, as <out>-dialog.bmp).
// --graph opens a procedural volume's graph in the node editor (M15),
// --node selects and previews one of its nodes (--slice: fields as a slice).
// --drawing opens a drawing by name (L6; a standard one of that name is made
// when the project has none); --drawing-image saves its sheet (rendered off
// screen) once drawn, and --screenshot then grabs the drawing window too, as
// <out>-drawing-window.bmp.
//
// With --screenshot or --viewport-image the app opens the project, waits
// for its evaluation to reach the screen, saves the window (as the screen
// shows it) or the viewport (rendered off screen) and quits: for the docs
// and the PR.

#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QScreen>
#include <QString>
#include <QTimer>

#include <filesystem>
#include <optional>
#include <thread>

#include "drawing_window.hpp"
#include "feature_dialog.hpp"
#include "main_window.hpp"
#include "node_editor.hpp"
#include "reference_choices.hpp"
#include "sketch_mode.hpp"
#include "viewport.hpp"
#include "vs/core/version.hpp"
#include "vs/gpu/gpu.hpp"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Volumetric CAD Studio"));
  QApplication::setOrganizationName(QStringLiteral("Volumetric Studio"));
  const auto version = vs::core::version_string();
  QApplication::setApplicationVersion(QString::fromUtf8(version.data(), static_cast<qsizetype>(version.size())));

  // Procedural fields go to the GPU once it has shown it samples exactly as
  // the CPU does (L1); the check runs beside start-up, and until it passes
  // (or if it never does) the CPU samples. --cpu skips it.
  std::thread gpu_check;
  if (!QApplication::arguments().contains(QStringLiteral("--cpu"))) {
    gpu_check = std::thread([] { vs::gpu::install(); });
  }
  // Last out (after the window and its session): the check finishes, then
  // the GPU device closes while the process is whole, never in static
  // destructors (H6).
  struct Join {
    std::thread& t;
    ~Join() {
      if (t.joinable()) t.join();
      vs::gpu::shutdown();
    }
  } join{gpu_check};

  QString project, select, view, screenshot, viewport_image, context, sketch, edit, graph, node, drawing,
      drawing_image;
  bool slice = false;
  std::optional<QSize> size;
  const QStringList args = QApplication::arguments();
  for (qsizetype i = 1; i < args.size(); ++i) {
    const QString& a = args[i];
    const bool has_value = i + 1 < args.size();
    if (a == QLatin1String("--select") && has_value) {
      select = args[++i];
    } else if (a == QLatin1String("--view") && has_value) {
      view = args[++i];
    } else if (a == QLatin1String("--screenshot") && has_value) {
      screenshot = args[++i];
    } else if (a == QLatin1String("--viewport-image") && has_value) {
      viewport_image = args[++i];
    } else if (a == QLatin1String("--context") && has_value) {
      context = args[++i];
    } else if (a == QLatin1String("--sketch") && has_value) {
      sketch = args[++i];
    } else if (a == QLatin1String("--graph") && has_value) {
      graph = args[++i];
    } else if (a == QLatin1String("--node") && has_value) {
      node = args[++i];
    } else if (a == QLatin1String("--drawing") && has_value) {
      drawing = args[++i];
    } else if (a == QLatin1String("--drawing-image") && has_value) {
      drawing_image = args[++i];
    } else if (a == QLatin1String("--cpu")) {
      // handled above
    } else if (a == QLatin1String("--slice")) {
      slice = true;
    } else if (a == QLatin1String("--edit") && has_value) {
      edit = args[++i];
    } else if (a == QLatin1String("--size") && has_value) {
      const QStringList wh = args[++i].split(QLatin1Char('x'));
      if (wh.size() == 2) size = QSize(wh[0].toInt(), wh[1].toInt());
    } else if (!a.startsWith(QLatin1String("--"))) {
      project = a;
    }
  }

  vs::app::MainWindow window;
  if (size) window.resize(*size);
  window.show();

  if (!project.isEmpty()) {
    window.open_project(std::filesystem::path(project.toStdWString()));
  }

  const bool capture = !screenshot.isEmpty() || !viewport_image.isEmpty() || !drawing_image.isEmpty();
  // A capture quits without asking about edits it made (--drawing may add a drawing).
  if (capture) window.set_unsaved_prompt([] { return vs::app::MainWindow::UnsavedChoice::Discard; });
  // A capture quits without asking about the edits it made (--drawing may add one).
  if (capture) window.set_unsaved_prompt([] { return vs::app::MainWindow::UnsavedChoice::Discard; });
  bool applied = false;
  // The dialog --edit opened; it may be closed (and deleted) by the time
  // the capture runs (H7).
  QPointer<vs::app::EditDialog> dialog;
  QObject::connect(&window, &vs::app::MainWindow::evaluationShown, &window, [&] {
    if (applied) return;
    applied = true;
    const auto snapshot = window.session().latest();
    if (!snapshot) return;
    const auto& doc = snapshot->evaluation.document;
    if (!context.isEmpty()) {
      for (const auto& v : doc.volumes) {
        if (v.name == context.toStdString()) window.edit_in_context(v.id);
      }
    }
    if (!select.isEmpty()) {
      const std::string name = select.toStdString();
      for (const auto& v : doc.volumes) {
        if (v.name == name) window.select(vs::app::Selection::of_volume(v.id));
      }
      for (const auto& c : doc.connections) {
        if (c.name == name) window.select(vs::app::Selection::of_surface(c.id));
      }
    }
    const auto* tree = vs::app::tree_of(doc, window.editing_volume());
    auto find = [&](const QString& label) -> const vs::feature::Feature* {
      if (tree == nullptr) return nullptr;
      for (const auto& f : tree->features()) {
        if (tree->label(f.id) == label.toStdString()) return &f;
      }
      return nullptr;
    };
    if (const auto* f = find(edit)) {
      dialog = window.edit_feature(window.editing_volume(), f->id);
      window.select(vs::app::Selection::of_feature(f->id, window.editing_volume()));
    }
    if (const auto* f = find(sketch)) window.edit_sketch(window.editing_volume(), f->id);
    const std::pair<const char*, vs::app::StandardView> views[] = {
        {"iso", vs::app::StandardView::Iso},     {"top", vs::app::StandardView::Top},
        {"bottom", vs::app::StandardView::Bottom}, {"front", vs::app::StandardView::Front},
        {"back", vs::app::StandardView::Back},   {"left", vs::app::StandardView::Left},
        {"right", vs::app::StandardView::Right}};
    for (const auto& [name, v] : views) {
      if (view == QLatin1String(name)) window.viewport().set_view(v);
    }
    if (!graph.isEmpty()) {
      for (const auto& v : doc.volumes) {
        if (v.name != graph.toStdString() || !window.edit_graph(v.id)) continue;
        if (slice) {
          if (auto* combo = window.node_editor().findChild<QComboBox*>()) combo->setCurrentIndex(1);
        }
        for (const auto& n : window.node_editor().graph().nodes) {
          if (n.name == node.toStdString()) window.select(vs::app::Selection::of_node(n.id, v.id));
        }
      }
    }
    if (window.sketch_mode() != nullptr) window.sketch_mode()->view().fit();
    // --drawing opens a drawing of the project by name, or makes a
    // standard one of that name when there is none (L6).
    bool drawing_open = false;
    if (!drawing.isEmpty()) {
      if (const auto* d = doc.find_drawing(drawing.toStdString())) {
        drawing_open = window.open_drawing(d->id);
      } else {
        drawing_open = !window.new_drawing(drawing.toStdString()).is_null();
      }
    }
    if (!capture) return;
    auto finish = [&, drawing_open] {
      if (!viewport_image.isEmpty()) window.viewport().render_image().save(viewport_image);
      if (!drawing_image.isEmpty()) {
        const QSize at = size ? *size : QSize(1600, 1131);
        window.drawing_view().render_image(at).save(drawing_image);
      }
      if (!screenshot.isEmpty()) {
        // A window off every screen has none.
        if (QScreen* screen = window.screen()) screen->grabWindow(window.winId()).save(screenshot);
        if (dialog) {
          QString out = screenshot;
          out.replace(QStringLiteral(".bmp"), QStringLiteral("-dialog.bmp"));
          if (QScreen* screen = dialog->screen()) screen->grabWindow(dialog->winId()).save(out);
        }
        if (drawing_open) {
          QString out = screenshot;
          out.replace(QStringLiteral(".bmp"), QStringLiteral("-drawing-window.bmp"));
          auto& dw = window.drawing_window();
          if (QScreen* screen = dw.screen()) screen->grabWindow(dw.winId()).save(out);
        }
      }
      QApplication::quit();
    };
    if (drawing_open) {
      // Wait for the drawing's sheet, then let it reach the screen.
      QObject::connect(&window, &vs::app::MainWindow::drawingShown, &window,
                       [&, finish] { QTimer::singleShot(1500, &window, finish); }, Qt::SingleShotConnection);
      return;
    }
    // Let the last frames reach the screen first.
    QTimer::singleShot(2500, &window, finish);
  });

  return QApplication::exec();
}

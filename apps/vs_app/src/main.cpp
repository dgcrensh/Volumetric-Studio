// vs_app: the Volumetric CAD Studio desktop application (M13, M14).
//
//   vs_app [project] [--select <volume or surface name>] [--view iso|top|front|right|...]
//          [--context <volume name>] [--sketch <sketch name>] [--edit <feature name>]
//          [--size <w>x<h>] [--screenshot <out.bmp>] [--viewport-image <out.bmp>]
//
// (vcpkg's Qt here is built without PNG support, so images are BMP.)
//
// --context edits a volume's contents in context; --sketch opens sketch
// mode on a sketch of the tree being edited; --edit opens a feature's
// dialog (with --screenshot it is saved too, as <out>-dialog.bmp).
//
// With --screenshot or --viewport-image the app opens the project, waits
// for its evaluation to reach the screen, saves the window (as the screen
// shows it) or the viewport (rendered off screen) and quits: for the docs
// and the PR.

#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QScreen>
#include <QString>
#include <QTimer>

#include <filesystem>
#include <optional>

#include "feature_dialog.hpp"
#include "main_window.hpp"
#include "reference_choices.hpp"
#include "sketch_mode.hpp"
#include "viewport.hpp"
#include "vs/core/version.hpp"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Volumetric CAD Studio"));
  QApplication::setOrganizationName(QStringLiteral("Volumetric Studio"));
  const auto version = vs::core::version_string();
  QApplication::setApplicationVersion(QString::fromUtf8(version.data(), static_cast<qsizetype>(version.size())));

  QString project, select, view, screenshot, viewport_image, context, sketch, edit;
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

  const bool capture = !screenshot.isEmpty() || !viewport_image.isEmpty();
  bool applied = false;
  vs::app::EditDialog* dialog = nullptr;
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
    if (window.sketch_mode() != nullptr) window.sketch_mode()->view().fit();
    if (!capture) return;
    // Let the last frames reach the screen first.
    QTimer::singleShot(2500, &window, [&] {
      if (!viewport_image.isEmpty()) window.viewport().render_image().save(viewport_image);
      if (!screenshot.isEmpty()) {
        window.screen()->grabWindow(window.winId()).save(screenshot);
        if (dialog != nullptr) {
          QString out = screenshot;
          out.replace(QStringLiteral(".bmp"), QStringLiteral("-dialog.bmp"));
          dialog->screen()->grabWindow(dialog->winId()).save(out);
        }
      }
      QApplication::quit();
    });
  });

  return QApplication::exec();
}

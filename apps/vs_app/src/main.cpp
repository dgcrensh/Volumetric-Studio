// vs_app: the Volumetric CAD Studio desktop application (M13).
//
//   vs_app [project] [--select <volume or surface name>] [--view iso|top|front|right|...]
//          [--size <w>x<h>] [--screenshot <out.bmp>] [--viewport-image <out.bmp>]
//
// (vcpkg's Qt here is built without PNG support, so images are BMP.)
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

#include "main_window.hpp"
#include "viewport.hpp"
#include "vs/core/version.hpp"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Volumetric CAD Studio"));
  QApplication::setOrganizationName(QStringLiteral("Volumetric Studio"));
  const auto version = vs::core::version_string();
  QApplication::setApplicationVersion(QString::fromUtf8(version.data(), static_cast<qsizetype>(version.size())));

  QString project, select, view, screenshot, viewport_image;
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
  QObject::connect(&window, &vs::app::MainWindow::evaluationShown, &window, [&] {
    if (applied) return;
    applied = true;
    const auto snapshot = window.session().latest();
    if (!select.isEmpty() && snapshot) {
      const auto& doc = snapshot->evaluation.document;
      const std::string name = select.toStdString();
      for (const auto& v : doc.volumes) {
        if (v.name == name) window.select(vs::app::Selection::of_volume(v.id));
      }
      for (const auto& c : doc.connections) {
        if (c.name == name) window.select(vs::app::Selection::of_surface(c.id));
      }
    }
    const std::pair<const char*, vs::app::StandardView> views[] = {
        {"iso", vs::app::StandardView::Iso},     {"top", vs::app::StandardView::Top},
        {"bottom", vs::app::StandardView::Bottom}, {"front", vs::app::StandardView::Front},
        {"back", vs::app::StandardView::Back},   {"left", vs::app::StandardView::Left},
        {"right", vs::app::StandardView::Right}};
    for (const auto& [name, v] : views) {
      if (view == QLatin1String(name)) window.viewport().set_view(v);
    }
    if (!capture) return;
    // Let the last frames reach the screen first.
    QTimer::singleShot(1500, &window, [&] {
      if (!viewport_image.isEmpty()) window.viewport().render_image().save(viewport_image);
      if (!screenshot.isEmpty()) {
        window.screen()->grabWindow(window.winId()).save(screenshot);
      }
      QApplication::quit();
    });
  });

  return QApplication::exec();
}

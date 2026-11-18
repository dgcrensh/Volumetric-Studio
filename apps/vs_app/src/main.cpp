// vs_app: placeholder main window until M13.

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QString>

#include "vs/core/version.hpp"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QMainWindow window;
  const auto version = vs::core::version_string();
  window.setWindowTitle(QStringLiteral("Volumetric CAD Studio ") +
                        QString::fromUtf8(version.data(), static_cast<qsizetype>(version.size())));
  window.setCentralWidget(new QLabel(QStringLiteral("Nothing here yet (M13).")));
  window.resize(1280, 800);
  window.show();
  return QApplication::exec();
}

// The vs_app suite's main: a QApplication for the session and window
// tests, then Catch2.

#include <QApplication>

#include <catch2/catch_session.hpp>

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  return Catch::Session().run(argc, argv);
}

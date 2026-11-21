#include "wait.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

namespace samples {

bool wait_until(const std::function<bool()>& done, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (!done()) {
    if (timer.elapsed() > timeout_ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  return true;
}

}  // namespace samples

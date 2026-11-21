#pragma once

// Waiting on Qt's event loop, for the vs_app (Qt) suite.

#include <functional>

namespace samples {

// Processes Qt events until `done` or the timeout; true when done.
bool wait_until(const std::function<bool()>& done, int timeout_ms);

}  // namespace samples

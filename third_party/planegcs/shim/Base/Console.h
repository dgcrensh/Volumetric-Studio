// vstudio shim for FreeCAD's Base/Console.h.
//
// PlaneGCS writes debug reports to FreeCAD's console. The studio reports
// solver results through vs_sketch's diagnostics instead, so these calls are
// swallowed.
#pragma once

namespace Base {

class ConsoleSingleton {
 public:
  template <typename... Args>
  void log(const Args&...) {}
  template <typename... Args>
  void message(const Args&...) {}
  template <typename... Args>
  void warning(const Args&...) {}
  template <typename... Args>
  void error(const Args&...) {}
};

inline ConsoleSingleton& Console() {
  static ConsoleSingleton console;
  return console;
}

}  // namespace Base

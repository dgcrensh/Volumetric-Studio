#pragma once

// Private: one GL context on a thread of its own, and the compute plumbing
// vs_gpu needs on it (programs, storage buffers, dispatches).
//
// Every call that touches GL runs as a job on the device's thread
// (run()); the caller waits for it. Programs are compiled once per source
// text and kept.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "gl.hpp"

namespace vs::gpu {

class Device {
 public:
  Device();
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  [[nodiscard]] bool ok() const { return ok_; }
  // Why there is no device (when !ok()).
  [[nodiscard]] const std::string& why() const { return why_; }
  // "NVIDIA L4, 4.6.0 NVIDIA 596.86": who answers.
  [[nodiscard]] const std::string& description() const { return description_; }

  // Runs `job` on the device's thread with its GL functions, and waits.
  void run(const std::function<void(const gl::Functions&)>& job);

  // On the device's thread (inside run()): the program for a compute
  // shader's source, compiled once; 0 with `log` set when it doesn't
  // compile or link.
  gl::GLuint program(const gl::Functions& f, const std::string& source, std::string& log);

 private:
  void loop();

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_ = false;
  bool ok_ = false;
  std::string why_;
  std::string description_;
  std::map<std::string, gl::GLuint> programs_;  // device thread only
  gl::Context* context_ = nullptr;
};

// A shader storage buffer on the device's thread: created, filled, bound,
// read back, deleted with the object.
class Buffer {
 public:
  Buffer(const gl::Functions& f, std::size_t bytes, const void* data = nullptr);
  ~Buffer();
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  void bind(gl::GLuint binding) const;
  void read(void* out, std::size_t bytes, std::size_t offset = 0) const;

 private:
  const gl::Functions& f_;
  gl::GLuint id_ = 0;
};

}  // namespace vs::gpu

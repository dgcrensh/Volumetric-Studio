#pragma once

// Private: one GL context on a thread of its own, and the compute plumbing
// vs_gpu needs on it (programs, storage buffers, dispatches).
//
// Every call that touches GL runs as a job on the device's thread
// (run()); the caller waits for it. A job that throws is rethrown to its
// caller; the device thread carries on. Programs are compiled once per
// source text and kept, at most kMaxPrograms of them (least recently used
// go first).

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
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
  static constexpr std::size_t kMaxPrograms = 64;

  Device();
  // Finishes the jobs already queued, deletes the programs and the
  // context, and joins the thread. Never call it from a job.
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  [[nodiscard]] bool ok() const { return ok_; }
  // Why there is no device (when !ok()).
  [[nodiscard]] const std::string& why() const { return why_; }
  // "NVIDIA L4, 4.6.0 NVIDIA 596.86": who answers.
  [[nodiscard]] const std::string& description() const { return description_; }
  // GL_MAX_SHADER_STORAGE_BLOCK_SIZE: the largest storage buffer a shader
  // may address, in bytes.
  [[nodiscard]] std::size_t max_block_bytes() const { return max_block_bytes_; }

  // Runs `job` on the device's thread with its GL functions, and waits.
  // False (job not run) when there is no device or it is stopping.
  // Exceptions from the job are rethrown here.
  bool run(const std::function<void(const gl::Functions&)>& job);

  // On the device's thread (inside run()): the program for a compute
  // shader's source, compiled once; 0 with `log` set when it doesn't
  // compile or link.
  gl::GLuint program(const gl::Functions& f, const std::string& source, std::string& log);
  // How many programs are kept (device thread, or for tests between jobs).
  [[nodiscard]] std::size_t program_count() const { return programs_.size(); }

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
  std::size_t max_block_bytes_ = 0;
  // Device thread only: programs by source, and their use order (front
  // least recent).
  std::list<std::string> program_order_;
  std::map<std::string, std::pair<gl::GLuint, std::list<std::string>::iterator>> programs_;
  gl::Context* context_ = nullptr;
};

// A shader storage buffer on the device's thread: created, filled, bound,
// read back, deleted with the object. A buffer given data is read-only for
// shaders (GL_STATIC_DRAW); one without is written and read back
// (GL_DYNAMIC_READ).
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

// Workgroup counts covering `groups` groups of a 1D job within GL's
// 65,535 per dimension: x as large as allowed, y as many rows as needed.
// Shaders take their index as gl_GlobalInvocationID.y * (gl_NumWorkGroups.x
// * local size) + gl_GlobalInvocationID.x and skip what is past the count.
struct Dispatch {
  gl::GLuint x = 1;
  gl::GLuint y = 1;
};
Dispatch dispatch_2d(std::size_t groups);

// Every pending GL error, as "0x0505, ..." (empty when none).
std::string gl_errors(const gl::Functions& f);

}  // namespace vs::gpu

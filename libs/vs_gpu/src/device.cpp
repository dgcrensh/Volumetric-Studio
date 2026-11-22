#include "device.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <memory>

#include <fmt/format.h>

namespace vs::gpu {

namespace {

constexpr gl::GLenum kMaxShaderStorageBlockSize = 0x90DE;

}  // namespace

Device::Device() {
  bool ready = false;
  thread_ = std::thread([this, &ready] {
    std::unique_ptr<gl::Context> context = gl::make_context(why_);
    {
      std::lock_guard lock(mutex_);
      if (context) {
        const auto& f = context->gl();
        const auto str = [&](gl::GLenum e) {
          const unsigned char* s = f.GetString(e);
          return s != nullptr ? std::string(reinterpret_cast<const char*>(s)) : std::string();
        };
        description_ = fmt::format("{}, {}", str(gl::kRenderer), str(gl::kVersion));
        gl::GLint block = 0;
        f.GetIntegerv(kMaxShaderStorageBlockSize, &block);
        // A negative answer would be a driver bug; 4.3 promises 16 MiB.
        max_block_bytes_ = block > 0 ? static_cast<std::size_t>(block) : std::size_t{1} << 24;
        while (f.GetError() != gl::kNoError) {
        }
        ok_ = true;
        context_ = context.get();
      }
      ready = true;
    }
    wake_.notify_all();
    if (context) loop();
    for (auto& [source, entry] : programs_) context->gl().DeleteProgram(entry.first);
    programs_.clear();
    program_order_.clear();
    {
      std::lock_guard lock(mutex_);
      context_ = nullptr;
    }
    // The context (and opengl32.dll) goes here, on the thread it is current on.
  });
  std::unique_lock lock(mutex_);
  wake_.wait(lock, [&] { return ready; });
}

Device::~Device() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Device::loop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    // run()'s wrapper catches the job's own exceptions; this keeps the
    // thread alive should anything else throw.
    try {
      job();
    } catch (...) {
    }
  }
}

bool Device::run(const std::function<void(const gl::Functions&)>& job) {
  // Shared with the job, so neither side outlives what it touches: the
  // job notifies while holding the waiter's mutex, and the waiter's state
  // lives as long as either needs it.
  struct Waiter {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::exception_ptr error;
  };
  const auto waiter = std::make_shared<Waiter>();
  {
    std::lock_guard lock(mutex_);
    if (!ok_ || stopping_ || context_ == nullptr) return false;
    jobs_.push_back([this, waiter, &job] {
      std::exception_ptr error;
      try {
        job(context_->gl());
      } catch (...) {
        error = std::current_exception();
      }
      std::lock_guard l(waiter->mutex);
      waiter->error = error;
      waiter->done = true;
      waiter->cv.notify_all();
    });
  }
  wake_.notify_all();
  std::unique_lock l(waiter->mutex);
  waiter->cv.wait(l, [&] { return waiter->done; });
  if (waiter->error) std::rethrow_exception(waiter->error);
  return true;
}

gl::GLuint Device::program(const gl::Functions& f, const std::string& source, std::string& log) {
  if (const auto it = programs_.find(source); it != programs_.end()) {
    program_order_.splice(program_order_.end(), program_order_, it->second.second);
    return it->second.first;
  }
  const gl::GLuint shader = f.CreateShader(gl::kComputeShader);
  const char* text = source.c_str();
  f.ShaderSource(shader, 1, &text, nullptr);
  f.CompileShader(shader);
  gl::GLint status = 0;
  f.GetShaderiv(shader, gl::kCompileStatus, &status);
  const auto info = [&](bool is_shader, gl::GLuint id) {
    gl::GLint length = 0;
    if (is_shader) {
      f.GetShaderiv(id, gl::kInfoLogLength, &length);
    } else {
      f.GetProgramiv(id, gl::kInfoLogLength, &length);
    }
    std::string s(static_cast<std::size_t>(std::max(length, 1)), '\0');
    if (is_shader) {
      f.GetShaderInfoLog(id, length, nullptr, s.data());
    } else {
      f.GetProgramInfoLog(id, length, nullptr, s.data());
    }
    return s;
  };
  if (status == 0) {
    log = info(true, shader);
    f.DeleteShader(shader);
    return 0;
  }
  const gl::GLuint program = f.CreateProgram();
  f.AttachShader(program, shader);
  f.LinkProgram(program);
  f.DeleteShader(shader);
  f.GetProgramiv(program, gl::kLinkStatus, &status);
  if (status == 0) {
    log = info(false, program);
    f.DeleteProgram(program);
    return 0;
  }
  // Least recently used out. Jobs take every program they need before
  // they dispatch, and none needs anywhere near kMaxPrograms, so this
  // never deletes one a running job holds.
  while (programs_.size() >= kMaxPrograms) {
    const auto oldest = programs_.find(program_order_.front());
    f.DeleteProgram(oldest->second.first);
    programs_.erase(oldest);
    program_order_.pop_front();
  }
  program_order_.push_back(source);
  programs_.emplace(source, std::pair{program, std::prev(program_order_.end())});
  return program;
}

Buffer::Buffer(const gl::Functions& f, std::size_t bytes, const void* data) : f_(f) {
  f_.GenBuffers(1, &id_);
  f_.BindBuffer(gl::kShaderStorageBuffer, id_);
  // Zero-sized buffers aren't allowed to bind; keep at least 16 bytes, and
  // never let GL read past the caller's shorter data.
  constexpr std::size_t kMin = 16;
  const gl::GLenum usage = data != nullptr ? gl::kStaticDraw : gl::kDynamicRead;
  if (bytes >= kMin) {
    f_.BufferData(gl::kShaderStorageBuffer, static_cast<gl::GLsizeiptr>(bytes), data, usage);
  } else {
    std::array<unsigned char, kMin> padded{};
    if (data != nullptr && bytes > 0) std::memcpy(padded.data(), data, bytes);
    f_.BufferData(gl::kShaderStorageBuffer, static_cast<gl::GLsizeiptr>(kMin), padded.data(), usage);
  }
}

Buffer::~Buffer() { f_.DeleteBuffers(1, &id_); }

void Buffer::bind(gl::GLuint binding) const { f_.BindBufferBase(gl::kShaderStorageBuffer, binding, id_); }

void Buffer::read(void* out, std::size_t bytes, std::size_t offset) const {
  f_.BindBuffer(gl::kShaderStorageBuffer, id_);
  f_.GetBufferSubData(gl::kShaderStorageBuffer, static_cast<gl::GLintptr>(offset),
                      static_cast<gl::GLsizeiptr>(bytes), out);
}

Dispatch dispatch_2d(std::size_t groups) {
  constexpr std::size_t kMaxGroups = 65535;
  Dispatch d;
  if (groups == 0) return d;
  d.x = static_cast<gl::GLuint>(std::min(groups, kMaxGroups));
  d.y = static_cast<gl::GLuint>((groups + d.x - 1) / d.x);
  return d;
}

std::string gl_errors(const gl::Functions& f) {
  std::string out;
  // GL keeps one flag per kind of error; a lost context can repeat one
  // forever, so stop after a few.
  for (int i = 0; i < 8; ++i) {
    const gl::GLenum e = f.GetError();
    if (e == gl::kNoError) break;
    out += fmt::format("{}0x{:04x}", out.empty() ? "" : ", ", e);
  }
  return out;
}

}  // namespace vs::gpu
